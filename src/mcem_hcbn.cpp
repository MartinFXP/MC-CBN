/** mccbn: large-scale inference on conjunctive Bayesian networks
 *  MCEM for the hidden conjuctive Bayesian network model
 *
 * @author Susana Posada Céspedes
 * @email susana.posada@bsse.ethz.ch
 */

#include <Rcpp.h>
#include <RcppEigen.h>
#include "mcem.hpp"
#include "not_acyclic_exception.hpp"
#include <boost/graph/graph_traits.hpp>
#include <random>
#include <vector>

#ifdef _OPENMP
  #include <omp.h>
#endif

using namespace Rcpp;
typedef Map<MatrixXd> MapMatd;

class Initializer {
public:
  Initializer() {
    Eigen::initParallel();
  }
} initializer;

void handle_exceptions() {
  try {
    throw;
  } catch (const std::exception& ex) {
    // NOTE: reference to 'exception' is ambiguous, 'std::' required
    std::string msg = std::string("c++ exception: ") + ex.what();
    ::Rf_error(msg.c_str());
  } catch (...) {
    ::Rf_error("c++ exception (unknown reason)");
  }
}

//' @noRd
//' @param N number of samples to be drawn
//' @param lambda rate
VectorXd rexp_std(const unsigned int N, const double lambda,
                  Context::rng_type& rng) {
  std::exponential_distribution<double> distribution(lambda);
  VectorXd T(N);

  for (unsigned int i = 0; i < N; ++i)
    T[i] = distribution(rng);

  return T;
}

//' @noRd
//' @param N number of samples to be drawn
std::vector<int> rdiscrete_std(const unsigned int N, const VectorXd weights,
                               std::mt19937& rng) {
  std::discrete_distribution<int> distribution(weights.data(),
                                               weights.data() + weights.size());
  std::vector<int> ret(N);

  for (unsigned int i = 0; i < N; ++i)
    ret[i] = distribution(rng);

  return ret;
}

//' @noRd
std::vector<int> runif_int_std(const unsigned int N, const int upper_limit,
                               std::mt19937& rng) {

  std::uniform_int_distribution<int> distribution(0, upper_limit);
  std::vector<int> ret(N);

  for (unsigned int i = 0; i < N; ++i)
    ret[i] = distribution(rng);

  return ret;
}

VectorXd log_bernoulli_process(const VectorXd& dist, const double eps,
                               const unsigned int p) {

  const unsigned int L = dist.size();
  VectorXd log_prob = VectorXd::Zero(L);

  if (eps == 0) {
    /* NOTE: If all observations are compatible with poset (which can happen
     * because noisy observations can be compatible), then eps can be 0, as
     * well as dist.
     */
    for (unsigned int i = 0; i < L; ++i) {
      if (dist[i] != 0) {
        log_prob[i] = log(eps + DBL_EPSILON) * dist[i] +
          log(1 - eps - DBL_EPSILON) * (p - dist[i]);
      } else {
        log_prob[i] = 0;
      }
    }
  } else {
    log_prob = (log(eps) * dist).array() + log(1 - eps) * (p - dist.array());
  }
  return log_prob;
}

//' Compute complete-data log-likelihood or (equivalently) hidden log-likelihood
double complete_log_likelihood(const VectorXd& lambda, const double eps,
                               const MatrixXd& Tdiff, const VectorXd& dist,
                               const float W) {

  const unsigned int p = lambda.size();
  const unsigned int N = dist.size();
  double llhood;

  llhood = W * lambda.array().log().sum() - (Tdiff * lambda).sum();
  if (eps == 0) {
    for (unsigned int i = 0; i < N; ++i) {
      if (dist(i) != 0)
        llhood += log(eps + DBL_EPSILON) * dist(i) +
          log(1 - eps - DBL_EPSILON) * (p - dist(i));
    }
  } else {
    llhood += log(eps) * dist.sum() + log(1 - eps) * (p - dist.array()).sum();
  }
  return llhood;
}

//' Generate observations from a given poset and given rates
//'
//' @noRd
//' @param N number of samples
//' @return returns matrix containing observations
MatrixXb sample_genotypes(
    const unsigned int N, const Model& model, MatrixXd& T_events,
    VectorXd& T_sampling, Context::rng_type& rng,
    const bool sampling_times_available=false) {

  // Initialization and instantiation of variables
  const vertices_size_type p = model.size();  // Number of mutations / events
  MatrixXd T_events_sum;
  MatrixXb obs;
  T_events_sum.setZero(N, p);
  obs.setConstant(N, p, false);

  /* Generate occurence times T_events_{j} ~ Exp(lambda_{j}) */
  for (unsigned int j = 0; j < p; ++j)
    T_events.col(j) = rexp_std(N, model.get_lambda(j), rng);

  /* Use sampling times when available */
  if (!sampling_times_available)
    T_sampling = rexp_std(N, model.get_lambda_s(), rng);

  /* Loop through nodes in topological order */
  for (node_container::const_reverse_iterator v = model.topo_path.rbegin();
       v != model.topo_path.rend(); ++v) {
    for (unsigned int i = 0; i < N; ++i) {
      double T_max = 0.0;
      /* Loop through parents of node v */
      boost::graph_traits<Poset>::in_edge_iterator in_begin, in_end;
      for (boost::tie(in_begin, in_end) = boost::in_edges(*v, model.poset);
           in_begin != in_end; ++in_begin)
        if (T_events_sum(i, source(*in_begin, model.poset)) > T_max)
          T_max = T_events_sum(i, source(*in_begin, model.poset));
      T_events_sum(i, *v) = T_events(i, *v) + T_max;
      if (T_events_sum(i, *v) <= T_sampling(i))
        obs(i, *v) = true;
    }
  }
  return obs;
}

//' Compute observed log-likelihood
double obs_log_likelihood(
    const MatrixXb& obs, const MatrixXi& poset, const VectorXd& lambda,
    const double eps, const VectorXd& times, const unsigned int L,
    const std::string& sampling, const unsigned int version, Context& ctx,
    const float lambda_s=1.0, const bool sampling_times_available=false,
    const unsigned int thrds=1) {

  const auto p = poset.rows(); // Number of mutations / events
  const auto N = obs.rows();   // Number of observations / genotypes
  double llhood = 0;

  edge_container edge_list = adjacency_mat2list(poset);
  Model model(edge_list, p, lambda_s);
  model.set_lambda(lambda);
  model.set_epsilon(eps);
  model.has_cycles();
  if (model.cycle) {
    throw not_acyclic_exception();
  } else {
    model.topological_sort();

    #ifdef _OPENMP
    omp_set_num_threads(thrds);
    #endif
    auto rngs = ctx.get_auxiliary_rngs(thrds);

    #pragma omp parallel for reduction(+:llhood) schedule(static)
    for (unsigned int i = 0; i < N; ++i) {
      DataImportanceSampling importance_sampling = importance_weight(
        obs.row(i), L, model, times[i], sampling, version,
        rngs[omp_get_thread_num()], sampling_times_available);
      llhood += std::log(importance_sampling.w.sum() / L);
    }
  }
  return llhood;
}

//' Compute Hamming distance between two vectors
//'
//' @noRd
//' @return returns Hamming distance
int hamming_dist(const VectorXi& x, const VectorXi& y) {
  return (x - y).array().abs().sum();
}

//' Compute Hamming distance between a matrix and a vector row-wise
//'
//' @noRd
//' @return returns a vector containing the Hamming distance
VectorXi hamming_dist_mat(const MatrixXb& x, const RowVectorXb& y) {
  const int N = x.rows();
  return (x.array() != y.replicate(N, 1).array()).rowwise().count().cast<int>();
  // return (x.rowwise() - y).array().abs().rowwise().sum();
}

//' Compute importance weights and (expected) sufficient statistics by
//' importance sampling
//'
//' @noRd
//' @param genotype a p-dimesional vector corresponding to an observed
//' genotype - e.g., mutated (1) and non-mutated (0) genes
//' @param L number of samples
//' @param model
//' @param time sampling time
//' @param sampling variable indicating which proposal to use
//' @return returns importance weights and (expected) sufficient statistics
DataImportanceSampling importance_weight(
    const RowVectorXb& genotype, const unsigned int L, const Model& model,
    const double time, const std::string& sampling, const unsigned int version,
    Context::rng_type& rng, const bool sampling_times_available=false) {

  // Initialization and instantiation of variables
  const vertices_size_type p = model.size(); // Number of mutations / events
  MatrixXb samples(L, p);
  DataImportanceSampling importance_sampling(L, p);

  if (sampling == "forward") {
    /* Generate L samples from poset with parameters lambda and lambda_s.
     * In particular, epsilon is zero (default value) - because the idea is to
     * generate samples of X (true genotype)
     */
    VectorXd T_sampling(L);
    if (sampling_times_available)
      T_sampling.setConstant(time);

    samples = sample_genotypes(L, model, importance_sampling.Tdiff, T_sampling,
                               rng, sampling_times_available);
    importance_sampling.dist = hamming_dist_mat(samples, genotype);
    VectorXd d = importance_sampling.dist.cast<double>();
    importance_sampling.w = pow(model.get_epsilon(), d.array()) *
      pow(1 - model.get_epsilon(), p - d.array());
  } else if (sampling == "add-remove") {
    //TODO
  } else if (sampling == "rejection") {
    unsigned int K = p * L;
    MatrixXd Tdiff_pool(K, p);
    VectorXd T_sampling(K);
    if (sampling_times_available)
      T_sampling.setConstant(time);

    MatrixXb genotype_pool = sample_genotypes(K, model, Tdiff_pool, T_sampling,
                                              rng, sampling_times_available);
    VectorXi dist_pool = hamming_dist_mat(genotype_pool, genotype);

    VectorXd d_pool = dist_pool.cast<double>();
    VectorXd q_prob = pow(model.get_epsilon(), d_pool.array()) *
      pow(1 - model.get_epsilon(), p - d_pool.array());
    /* In the unlikely event that q_prob is 0 for all samples, default to
     * random sampling
     */
    bool random = false;
    if (q_prob.sum() == 0) {
      q_prob.setConstant(1);
      random = true;
    }
    double q_prob_sum = q_prob.sum();
    q_prob /= q_prob_sum;

    // Draw L samples with replacement and with weights q_prob
    std::vector<int> idxs_sample = rdiscrete_std(L, q_prob, rng);
    int idx;
    for (unsigned int l = 0; l < L; ++l) {
      idx = idxs_sample[l];
      importance_sampling.dist(l) = dist_pool(idx);
      importance_sampling.Tdiff.row(l) = Tdiff_pool.row(idx);
    }

    if (random)
      importance_sampling.w =
        log_bernoulli_process(importance_sampling.dist.cast<double>(),
                              model.get_epsilon(), p).array().exp();
    else
      importance_sampling.w.setConstant(q_prob_sum / dist_pool.size());
  }

  return importance_sampling;
}

//' Compute importance weights and sufficient statistics by sampling
//'
//' @noRd
double MCEM_hcbn(
    Model& model, const MatrixXb& obs, const VectorXd& times,
    const RowVectorXd& weights, unsigned int L, const std::string& sampling,
    const unsigned int version, const ControlEM& control_EM,
    const bool sampling_times_available, const unsigned int thrds,
    Context& ctx) {

  // Initialization and instantiation of variables
  const vertices_size_type p = model.size(); // Number of mutations / events
  const unsigned int N = obs.rows();         // Number of observations / genotypes
  float W = weights.sum();                   // Number of (weighted) observations

  unsigned int update_step_size = control_EM.update_step_size;
  VectorXd avg_lambda = VectorXd::Zero(p);
  VectorXd avg_lambda_current = VectorXd::Zero(p);
  double avg_eps = 0, avg_llhood = 0, llhood = 0;
  double avg_eps_current = 0;
  bool tol_comparison = true;
  VectorXd expected_dist(N);
  MatrixXd expected_Tdiff(N, p);
  VectorXd Tdiff_colsum(p);

  /*if (sampling == "rejection") {
    // if (p < 19) {
    //   K = std::max((unsigned int) pow(2, p + 1), 2 * L);
    // } else {
    //   // K = 100000000 / (sizeof(double) * p); //NOTE:empirically 100000 limit on runtime
    //   K = p * L;
    // }
    K = p * L;
    if (ctx.get_verbose())
      std::cout << "Size of the genotype pool: " << K << std::endl;
  }*/

  if (ctx.get_verbose()) {
    std::cout << "Initial value of the error rate - epsilon: "
              << model.get_epsilon() << std::endl;
    std::cout << "Initial value of the rate parameters - lambda: "
              << model.get_lambda().transpose() << std::endl;
  }

  for (unsigned int iter = 0; iter < control_EM.max_iter; ++iter) {

    if (iter == update_step_size) {
      avg_lambda_current /= control_EM.update_step_size;
      avg_eps_current /= control_EM.update_step_size;
      avg_llhood /= control_EM.update_step_size;
      if (tol_comparison) {
        if (std::abs(avg_eps - avg_eps_current) <= control_EM.tol &&
            ((avg_lambda - avg_lambda_current).array().abs() <= control_EM.tol).all())
          break;
        // L *= 2;
      }
      avg_lambda = avg_lambda_current;
      avg_eps = avg_eps_current;

      update_step_size += control_EM.update_step_size;
      tol_comparison = true;

      /* Restart averaging */
      avg_lambda_current = VectorXd::Zero(p);
      avg_eps_current = 0;
      avg_llhood = 0;
    }

    /* E step
     * Conditional expectation for the sufficient statistics per observation
     * and event
     */
    #ifdef _OPENMP
      omp_set_num_threads(thrds);
    #endif
    auto rngs = ctx.get_auxiliary_rngs(thrds);

    #pragma omp parallel for schedule(static)
    for (unsigned int i = 0; i < N; ++i) {
      DataImportanceSampling importance_sampling = importance_weight(
        obs.row(i), L, model, times(i), sampling, version,
        rngs[omp_get_thread_num()], sampling_times_available);

      expected_dist(i) =
        importance_sampling.w.dot(importance_sampling.dist.cast<double>()) /
          importance_sampling.w.sum();
      expected_Tdiff.row(i) =
        (importance_sampling.Tdiff.transpose() * importance_sampling.w) /
        importance_sampling.w.sum();
    }

    /* M-step */
    model.set_epsilon(expected_dist.sum() / (N * p));
    Tdiff_colsum = weights * expected_Tdiff;
    model.set_lambda((Tdiff_colsum / W).array().inverse(), control_EM.max_lambda);

    llhood = complete_log_likelihood(
      model.get_lambda(), model.get_epsilon(), expected_Tdiff, expected_dist,
      W);

    avg_lambda_current +=  model.get_lambda();
    avg_eps_current += model.get_epsilon();
    avg_llhood += llhood;

    if (iter + 1 == control_EM.max_iter) {
      unsigned int num_iter = control_EM.max_iter - update_step_size +
        control_EM.update_step_size;
      avg_lambda_current /= num_iter;
      avg_eps_current /= num_iter;
      avg_llhood /= num_iter;
    }

    if (ctx.get_verbose()) {
      if (iter == 0)
        std::cout << "llhood\tepsilon\tlambdas" << std::endl;
      std::cout << llhood << "\t" << model.get_epsilon() << "\t"
                << model.get_lambda().transpose() << std::endl;
    }
  }

  model.set_lambda(avg_lambda_current);
  model.set_epsilon(avg_eps_current);
  model.set_llhood(avg_llhood);

  return avg_llhood;
}

RcppExport SEXP _complete_log_likelihood(
    SEXP lambdaSEXP, SEXP epsSEXP, SEXP TdiffSEXP, SEXP distSEXP, SEXP WSEXP) {

  try {
    // Convert input to C++ types
    const MapVecd lambda(as<MapVecd>(lambdaSEXP));
    double eps = as<double>(epsSEXP);
    const MapMatd Tdiff(as<MapMatd>(TdiffSEXP));
    const MapVecd dist(as<MapVecd>(distSEXP));
    float W = as<float>(WSEXP);

    // Call the underlying C++ function
    double res = complete_log_likelihood(lambda, eps, Tdiff, dist, W);

    // Return the result as a SEXP
    return wrap( res );
  } catch  (...) {
    handle_exceptions();
  }
  return R_NilValue;
}

RcppExport SEXP _obs_log_likelihood(
    SEXP obsSEXP, SEXP posetSEXP, SEXP lambdaSEXP, SEXP epsSEXP,
    SEXP timesSEXP, SEXP LSEXP, SEXP samplingSEXP, SEXP versionSEXP,
    SEXP lambda_sSEXP, SEXP sampling_times_availableSEXP, SEXP thrdsSEXP,
    SEXP seedSEXP) {

  try {
    // Convert input to C++ types
    const MatrixXb& obs = as<MatrixXb>(obsSEXP);
    const MapMati poset(as<MapMati>(posetSEXP));
    const MapVecd lambda(as<MapVecd>(lambdaSEXP));
    const double eps = as<double>(epsSEXP);
    const MapVecd times(as<MapVecd>(timesSEXP));
    const unsigned int L = as<unsigned int>(LSEXP);
    const std::string& sampling = as<std::string>(samplingSEXP);
    const unsigned int version = as<unsigned int>(versionSEXP);
    const float lambda_s = as<float>(lambda_sSEXP);
    const bool sampling_times_available = as<bool>(sampling_times_availableSEXP);
    const int thrds = as<int>(thrdsSEXP);
    const int seed = as<int>(seedSEXP);

    // Call the underlying C++ function
    Context ctx(seed);
    double llhood = obs_log_likelihood(
      obs, poset, lambda, eps, times, L, sampling, version, ctx, lambda_s,
      sampling_times_available, thrds);

    // Return the result as a SEXP
    return wrap( llhood );
  } catch  (...) {
    handle_exceptions();
  }
  return R_NilValue;
}

//' @noRd
//' @param update_step_sizeSEXP Evaluate convergence of parameter every
//' 'update_step_size' and increase number of samples, 'L', in order to reach a
//' desirable 'tol'
//' @param tolSEXP Convergence tolerance for rate paramenters
RcppExport SEXP _MCEM_hcbn(
    SEXP ilambdaSEXP, SEXP posetSEXP, SEXP obsSEXP, SEXP timesSEXP,
    SEXP lambda_sSEXP, SEXP epsSEXP, SEXP weightsSEXP, SEXP LSEXP,
    SEXP samplingSEXP, SEXP versionSEXP, SEXP max_iterSEXP,
    SEXP update_step_sizeSEXP, SEXP tolSEXP, SEXP max_lambdaSEXP,
    SEXP sampling_times_availableSEXP, SEXP thrdsSEXP, SEXP verboseSEXP,
    SEXP seedSEXP) {

  try {
    // Convert input to C++ types
    VectorXd ilambda = as<MapVecd>(ilambdaSEXP);
    const MapMati poset(as<MapMati>(posetSEXP));
    const MatrixXb& obs = as<MatrixXb>(obsSEXP);
    const MapVecd times(as<MapVecd>(timesSEXP));
    const float lambda_s = as<float>(lambda_sSEXP);
    const double eps = as<double>(epsSEXP);
    const MapRowVecd weights(as<MapRowVecd>(weightsSEXP));
    unsigned int L = as<unsigned int>(LSEXP);
    const std::string& sampling = as<std::string>(samplingSEXP);
    const unsigned int version = as<unsigned int>(versionSEXP);
    const unsigned int max_iter = as<unsigned int>(max_iterSEXP);
    const unsigned int update_step_size = as<unsigned int>(update_step_sizeSEXP);
    const double tol = as<double>(tolSEXP);
    const float max_lambda = as<float>(max_lambdaSEXP);
    const bool sampling_times_available = as<bool>(sampling_times_availableSEXP);
    const int thrds = as<int>(thrdsSEXP);
    const bool verbose = as<bool>(verboseSEXP);
    const int seed = as<int>(seedSEXP);

    const auto p = poset.rows(); // Number of mutations / events
    edge_container edge_list = adjacency_mat2list(poset);
    Model M(edge_list, p, lambda_s);
    M.set_lambda(ilambda);
    M.set_epsilon(eps);
    M.has_cycles();
    if (M.cycle)
      throw not_acyclic_exception();
    M.topological_sort();

    ControlEM control_EM(max_iter, update_step_size, tol, max_lambda);

    // Call the underlying C++ function
    Context ctx(seed, verbose);
    double llhood = MCEM_hcbn(
      M, obs, times, weights, L, sampling, version, control_EM,
      sampling_times_available, thrds, ctx);

    // Return the result as a SEXP
    return List::create(_["lambda"]=M.get_lambda(), _["eps"]=M.get_epsilon(),
                        _["llhood"]=llhood);
  } catch  (...) {
    handle_exceptions();
  }
  return R_NilValue;
}

RcppExport SEXP _importance_weight_genotype(
    SEXP genotypeSEXP, SEXP LSEXP, SEXP posetSEXP, SEXP lambdaSEXP,
    SEXP epsSEXP, SEXP timeSEXP, SEXP samplingSEXP, SEXP versionSEXP,
    SEXP lambda_sSEXP, SEXP sampling_times_availableSEXP, SEXP seedSEXP) {

  try {
    // Convert input to C++ types
    const RowVectorXb& genotype = as<RowVectorXb>(genotypeSEXP);
    const unsigned int L = as<unsigned int>(LSEXP);
    const MapMati poset(as<MapMati>(posetSEXP));
    const MapVecd lambda(as<MapVecd>(lambdaSEXP));
    const double eps = as<double>(epsSEXP);
    const double time = as<double>(timeSEXP);
    const std::string& sampling = as<std::string>(samplingSEXP);
    const unsigned int version = as<unsigned int>(versionSEXP);
    const float lambda_s = as<float>(lambda_sSEXP);
    const bool sampling_times_available = as<bool>(sampling_times_availableSEXP);
    const int seed = as<int>(seedSEXP);

    const auto p = poset.rows(); // Number of mutations / events
    edge_container edge_list = adjacency_mat2list(poset);
    Model M(edge_list, p, lambda_s);
    M.set_lambda(lambda);
    M.set_epsilon(eps);
    M.has_cycles();
    if (M.cycle)
      throw not_acyclic_exception();
    M.topological_sort();

    // Call the underlying C++ function
    Context ctx(seed);
    DataImportanceSampling w = importance_weight(
      genotype, L, M, time, sampling, version, ctx.rng,
      sampling_times_available);

    // Return the result as a SEXP
    return List::create(_["w"]=w.w, _["dist"]=w.dist, _["Tdiff"]=w.Tdiff);
  } catch  (...) {
    handle_exceptions();
  }
  return R_NilValue;
}

RcppExport SEXP _importance_weight(
    SEXP obsSEXP, SEXP LSEXP, SEXP posetSEXP, SEXP lambdaSEXP,
    SEXP epsSEXP, SEXP timesSEXP, SEXP samplingSEXP, SEXP versionSEXP,
    SEXP lambda_sSEXP, SEXP sampling_times_availableSEXP, SEXP thrdsSEXP,
    SEXP seedSEXP) {

  try {
    // Convert input to C++ types
    const MatrixXb& obs = as<MatrixXb>(obsSEXP);
    const unsigned int L = as<unsigned int>(LSEXP);
    const MapMati poset(as<MapMati>(posetSEXP));
    const MapVecd lambda(as<MapVecd>(lambdaSEXP));
    const double eps = as<double>(epsSEXP);
    const MapVecd times(as<MapVecd>(timesSEXP));
    const std::string& sampling = as<std::string>(samplingSEXP);
    const unsigned int version = as<unsigned int>(versionSEXP);
    const float lambda_s = as<float>(lambda_sSEXP);
    const bool sampling_times_available = as<bool>(sampling_times_availableSEXP);
    const int thrds = as<int>(thrdsSEXP);
    const int seed = as<int>(seedSEXP);

    const unsigned int N = obs.rows(); // Number of observations / genotypes
    const auto p = poset.rows();       // Number of mutations / events

    VectorXd w_sum(N);
    VectorXd expected_dist(N);
    MatrixXd expected_Tdiff(N, p);
    edge_container edge_list = adjacency_mat2list(poset);
    Model M(edge_list, p, lambda_s);
    M.set_lambda(lambda);
    M.set_epsilon(eps);
    M.has_cycles();
    if (M.cycle)
      throw not_acyclic_exception();
    M.topological_sort();

    Context ctx(seed);

    #ifdef _OPENMP
    omp_set_num_threads(thrds);
    #endif
    auto rngs = ctx.get_auxiliary_rngs(thrds);

    #pragma omp parallel for schedule(static)
    for (unsigned int i = 0; i < N; ++i) {
      // Call the underlying C++ function
      DataImportanceSampling w = importance_weight(
        obs.row(i), L, M, times(i), sampling, version,
        rngs[omp_get_thread_num()], sampling_times_available);

      w_sum[i] = w.w.sum();
      expected_dist[i] = w.w.dot(w.dist.cast<double>()) / w_sum[i];
      expected_Tdiff.row(i) = (w.Tdiff.transpose() * w.w) / w_sum[i];
    }

    // Return the result as a SEXP
    return List::create(_["w"]=w_sum, _["dist"]=expected_dist,
                        _["Tdiff"]=expected_Tdiff);
  } catch  (...) {
    handle_exceptions();
  }
  return R_NilValue;
}

RcppExport SEXP _sample_genotypes(
    SEXP NSEXP, SEXP posetSEXP, SEXP lambdaSEXP, SEXP T_eventsSEXP,
    SEXP T_samplingSEXP, SEXP lambda_sSEXP, SEXP sampling_times_availableSEXP,
    SEXP seedSEXP) {

  try {
    // Convert input to C++ types
    const unsigned int N = as<unsigned int>(NSEXP);
    const MapMati poset(as<MapMati>(posetSEXP));
    const MapVecd lambda(as<MapVecd>(lambdaSEXP));
    MatrixXd T_events = as<MapMatd>(T_eventsSEXP);
    VectorXd T_sampling = as<MapVecd>(T_samplingSEXP);
    const float lambda_s = as<float>(lambda_sSEXP);
    const bool sampling_times_available = as<bool>(sampling_times_availableSEXP);
    const int seed = as<int>(seedSEXP);

    const auto p = poset.rows(); // Number of mutations / events
    edge_container edge_list = adjacency_mat2list(poset);
    Model M(edge_list, p, lambda_s);
    M.set_lambda(lambda);
    M.has_cycles();
    if (M.cycle)
      throw not_acyclic_exception();
    M.topological_sort();

    // Call the underlying C++ function
    Context ctx(seed);
    MatrixXb samples = sample_genotypes(N, M, T_events, T_sampling, ctx.rng,
                                        sampling_times_available);

    // Return the result as a SEXP
    return List::create(_["samples"]=samples, _["Tdiff"]=T_events,
                        _["sampling_time"]=T_sampling);
  } catch  (...) {
    handle_exceptions();
  }
  return R_NilValue;
}
