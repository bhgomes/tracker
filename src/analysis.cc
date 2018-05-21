#include "analysis.hh"

#include <deque>
#include <numeric>

#include "ROOT/TMinuit.h"

#include "geometry.hh"
#include "units.hh"

#include "util/bit_vector.hh"
#include "util/io.hh"
#include "util/math.hh"

namespace MATHUSLA { namespace TRACKER {

namespace analysis { ///////////////////////////////////////////////////////////////////////////

//__Average Point_______________________________________________________________________________
const r4_point mean(const event_points& points) {
  const auto&& size = points.size();
  return (size == 0) ? r4_point{} : std::accumulate(points.cbegin(), points.cend(), r4_point{}) / size;
}
//----------------------------------------------------------------------------------------------

//__Row-Major Covariance Matrix_________________________________________________________________
const r4_point_vector covariance_matrix(const event_points& points) {
  const auto&& size = points.size();
  if (size == 0) return {};

  const auto&& inv_size = 1.0L / size;

  const auto& average = inv_size * std::accumulate(points.cbegin(), points.cend(), r4_point{});
  const auto& points_T = transpose(points);

  const real&& covTT = inv_size * (points_T.ts * points_T.ts) - average.t * average.t;
  const real&& covXX = inv_size * (points_T.xs * points_T.xs) - average.x * average.x;
  const real&& covYY = inv_size * (points_T.ys * points_T.ys) - average.y * average.y;
  const real&& covZZ = inv_size * (points_T.zs * points_T.zs) - average.z * average.z;

  const real&& covTX = inv_size * (points_T.ts * points_T.xs) - average.t * average.x;
  const real&& covTY = inv_size * (points_T.ts * points_T.ys) - average.t * average.y;
  const real&& covTZ = inv_size * (points_T.ts * points_T.zs) - average.t * average.z;

  const real&& covXY = inv_size * (points_T.xs * points_T.ys) - average.x * average.y;
  const real&& covXZ = inv_size * (points_T.xs * points_T.zs) - average.x * average.z;

  const real&& covYZ = inv_size * (points_T.ys * points_T.zs) - average.y * average.z;

  return {{covTT, covTX, covTY, covTZ},
          {covTX, covXX, covXY, covXZ},
          {covTY, covXY, covYY, covYZ},
          {covTZ, covXZ, covYZ, covZZ}};
}
//----------------------------------------------------------------------------------------------

//__Collapse Points by R4 Interval______________________________________________________________
event_points collapse(const event_points& event,
                      const r4_point& ds) {
  if (event.empty())
    return event;

  event_points out;

  const auto& sorted_event = t_copy_sort(event);
  const auto&& size = sorted_event.size();

  using size_type = event_points::size_type;

  size_type index = 0;
  std::deque<size_type> marked_indicies{};
  while (index < size) {
    size_type collected = 1, missed_index = 0;
    const auto& point = sorted_event[index];
    const auto&& time_interval = point.t + ds.t;

    r4_point sum = point;

    auto skipped = false;
    while (++index < size) {

      while (!marked_indicies.empty() && index++ == marked_indicies.front())
        marked_indicies.pop_front();

      const auto& next = sorted_event[index];
      if (next.t > time_interval)
        break;

      if (within_dr(point, next, ds)) {
        ++collected;
        sum += next;
        if (skipped)
          marked_indicies.push_back(index);
      } else if (!skipped) {
        skipped = true;
        missed_index = index;
      }
    }

    if (skipped)
      index = missed_index;

    out.push_back(sum / collected);
  }

  return out;
}
//----------------------------------------------------------------------------------------------

//__Partition Points by Coordinate______________________________________________________________
event_partition partition(const event_points& points,
                          const real interval,
                          const Coordinate coordinate) {
  event_partition out{{}, coordinate};
  if (points.empty())
    return out;

  auto& parts = out.parts;

  const auto& sorted_points = coordinate_stable_copy_sort(points, coordinate);
  const auto&& size = sorted_points.size();

  event_points::size_type count = 0;
  auto point_iter = sorted_points.cbegin();
  while (count < size) {
    const auto& point = *point_iter;
    event_points current_layer{point};
    ++count;

    while (count < size) {
      const auto& next = *(++point_iter);
      if ((coordinate == Coordinate::T && (next.t > point.t + interval)) ||
          (coordinate == Coordinate::X && (next.x > point.x + interval)) ||
          (coordinate == Coordinate::Y && (next.y > point.y + interval)) ||
          (coordinate == Coordinate::Z && (next.z > point.z + interval))) {
        break;
      }
      current_layer.push_back(next);
      ++count;
    }

    parts.push_back(t_sort(current_layer));
  }

  return out;
}
//----------------------------------------------------------------------------------------------

//__Fast Check if Points Form a Line____________________________________________________________
bool fast_line_check(const event_points& points, const real threshold) {
  const auto& line_begin = points.front();
  const auto& line_end = points.back();
  return threshold >= std::accumulate(points.cbegin() + 1, points.cend() - 1, threshold,
    [&](const auto& max, const auto& point) {
        return std::max(max, point_line_distance(point, line_begin, line_end)); });
}
//----------------------------------------------------------------------------------------------

//__Seeding Algorithm___________________________________________________________________________
event_vector seed(const size_t n,
                  const event_points& event,
                  const r4_point& collapse_ds,
                  const real layer_dz,
                  const real line_dr) {
  if (n <= 2) return {};

  const auto& points = collapse(event, collapse_ds);
  const auto&& size = points.size();

  if (size <= n) return { points };

  event_vector out;
  out.reserve(std::pow(size, n) / std::pow(n/2.718, n));  // FIXME: work on this limit (Stirling's approximation)

  const auto& layers = partition(points, layer_dz).parts;
  const auto&& layer_count = layers.size();
  if (layer_count < n) return {}; // FIXME: unsure what to do here

  util::bit_vector_sequence layer_sequence;
  for (const auto& layer : layers)
    layer_sequence.emplace_back(1, layer.size());

  util::order2_permutations(n, layer_sequence, [&](const auto& chooser) {
    event_points tuple;
    tuple.reserve(n);

    for (size_t index = 0; index < layer_count; ++index) {
      if (chooser[index]) {
        const auto& layer = layers[index];
        const auto& bits = layer_sequence[index];
        size_t j = 0;
        std::copy_if(layer.cbegin(), layer.cend(), std::back_inserter(tuple),
          [&](auto) { return bits[j++]; });
      }
    }

    if (fast_line_check(t_sort(tuple), line_dr))
      out.push_back(tuple);
  });

  return out;
}
//----------------------------------------------------------------------------------------------

//__Check if Seeds can be Joined________________________________________________________________
bool seeds_compatible(const event_points& first, const event_points& second, const size_t difference) {
  return std::equal(first.cbegin() + difference, first.cend(), second.cbegin(), second.cend() - difference);
}
//----------------------------------------------------------------------------------------------

//__Join Two Seeds______________________________________________________________________________
event_points join(const event_points& first, const event_points& second, const size_t difference) {
  const auto&& first_size = first.size();
  const auto&& second_size = second.size();
  const auto&& overlap = first_size - difference;

  if (overlap <= 0 || second_size < overlap)
    return {};

  const auto&& size = difference + second_size;
  event_points out;
  out.reserve(size);

  size_t index = 0;
  for (; index < difference; ++index) out.push_back(first[index]);
  for (; index < difference + overlap; ++index) {
    const auto& point = first[index];
    if (point != second[index - difference])
      return {};
    out.push_back(point);
  }
  for (; index < size; ++index) out.push_back(second[index - difference]);

  return out;
}
//----------------------------------------------------------------------------------------------

namespace { ////////////////////////////////////////////////////////////////////////////////////

//__Target Range________________________________________________________________________________
struct _range { size_t begin, end; };
//----------------------------------------------------------------------------------------------

//__Find Target Range for Secondaries___________________________________________________________
const _range _find_range(const size_t seed_index,
                         const size_t difference,
                         const event_vector& seeds,
                         const std::vector<size_t>& change_list) {
  const auto& seed_second = seeds[seed_index][difference];
  const auto&& size = change_list.size();
  size_t begin, end = change_list[size - 1], index = 1;
  while (true) {
    begin = change_list[index++];
    if (begin == end) break;
    if (seed_second != seeds[begin].front()) continue;
    end = change_list[index];
    break;
  }
  return { std::move(begin), std::move(end) };
}
//----------------------------------------------------------------------------------------------

//__Join all Secondaries to a Seed______________________________________________________________
void _join_secondaries(const size_t seed_index,
                       const size_t difference,
                       const _range range,
                       const event_vector& seeds,
                       util::bit_vector& join_list,
                       event_vector& out) {
  const auto& seed = seeds[seed_index];
  const auto&& range_end = std::move(range.end);
  for (auto index = range.begin; index < range_end; ++index) {
    const auto& joined = join(seed, seeds[index], difference);
    if (!joined.empty()) {
      out.push_back(joined);
      join_list[index] = true;
      join_list[seed_index] = true;
    }
  }
}
//----------------------------------------------------------------------------------------------

//__Partial Join________________________________________________________________________________
const event_vector _partial_join(const event_vector& seeds,
                                 const size_t difference,
                                 event_vector& singular) {
  const auto&& size = seeds.size();

  std::vector<size_t> change_list;
  change_list.push_back(0);
  for (size_t index = 1, last_index = 0; index < size; ++index) {
    if (seeds[index].front() != seeds[last_index].front()) {
      change_list.push_back(index);
      last_index = index;
    }
  }
  change_list.push_back(size);
  change_list.shrink_to_fit();

  util::bit_vector join_list(size);

  event_vector out;
  out.reserve(size);
  for (size_t index = 0; index < size; ++index) {
    _join_secondaries(
      index,
      difference,
      _find_range(index, difference, seeds, change_list),
      seeds,
      join_list,
      out);
  }

  for (size_t index = 0; index < size; ++index)
    if (!join_list[index])
      singular.push_back(seeds[index]);

  return out;
}
//----------------------------------------------------------------------------------------------

} /* anonymous namespace */ ////////////////////////////////////////////////////////////////////

//__Seed Join___________________________________________________________________________________
event_vector join_all(const event_vector& seeds) {
  event_vector out;
  out.reserve(seeds.size());
  auto out_inserter = std::back_inserter(out);

  auto current_seeds = seeds;
  while (true) {
    const auto& size = current_seeds.size();
    current_seeds = _partial_join(current_seeds, 1, out);
    if (current_seeds.size() == size) break;
  }
  std::copy(current_seeds.cbegin(), current_seeds.cend(), out_inserter);

  std::cout << "seeds: " << seeds.size() << "\n";
  /*
  for (const auto& seed : seeds)
    util::io::print_range(seed) << "\n";
  */

  std::cout << "out: " << out.size() << "\n";
  /*
  for (const auto& seed : out)
    util::io::print_range(seed) << "\n";
  */

  return out;
}
//----------------------------------------------------------------------------------------------

namespace { ////////////////////////////////////////////////////////////////////////////////////

//__Calculate Squared Residual of Track Through a Volume________________________________________
real _track_squared_residual(const real t0,
                             const real x0,
                             const real y0,
                             const real z0,
                             const real vx,
                             const real vy,
                             const real vz,
                             const std::string& volume) {
  const auto& limits = geometry::limits_of(volume);
  const auto& center = limits.center;
  const auto& min = limits.min;
  const auto& max = limits.max;
  const auto&& dz = (center.z - z0) / vz;
  const auto&& t_res = (dz + t0) / (2 * units::time);
  const auto&& x_res = (std::fma(dz, vx, x0) - center.x) / (max.x - min.x);
  const auto&& y_res = (std::fma(dz, vy, y0) - center.y) / (max.y - min.y);
  return t_res*t_res + 12*x_res*x_res + 12*y_res*y_res;
}
//----------------------------------------------------------------------------------------------

//__Track Parameter Type________________________________________________________________________
struct _track_parameters { fit_parameter t0, x0, y0, z0, vx, vy, vz; };
//----------------------------------------------------------------------------------------------

//__Fast Guess of Initial Track Parameters______________________________________________________
_track_parameters _guess_track(const event_points& event) {
  const auto& first = event.front();
  const auto& last = event.back();
  const auto&& dt = last.t - first.t;
  return {{first.t, 2*units::time, 0, 0},
          {first.x, 100*units::length, 0, 0},
          {first.y, 100*units::length, 0, 0},
          {first.z, 100*units::length, 0, 0},
          {(last.x - first.x) / dt, 0.1*units::speed_of_light, 0, units::speed_of_light},
          {(last.y - first.y) / dt, 0.1*units::speed_of_light, 0, units::speed_of_light},
          {(last.z - first.z) / dt, 0.1*units::speed_of_light, 0, units::speed_of_light}};
}
//----------------------------------------------------------------------------------------------

//__Gaussian Negative Log Likelihood Calculation________________________________________________
thread_local event_points&& _nll_fit_event = {};
void _gaussian_nll(Int_t&, Double_t*, Double_t& out, Double_t* parameters, Int_t) {
  out = 0.5L * std::accumulate(_nll_fit_event.cbegin(), _nll_fit_event.cend(), 0.0L,
    [&](const auto& sum, const auto& point) {
      return sum + _track_squared_residual(
        parameters[0],
        parameters[1],
        parameters[2],
        parameters[3],
        parameters[4],
        parameters[5],
        parameters[6],
        geometry::volume(point)); });
  //std::cout << "nll: " << out << "\n";
}
//----------------------------------------------------------------------------------------------

//__MINUIT Gaussian Fitter______________________________________________________________________
_track_parameters& _fit_event(const event_points& event,
                              _track_parameters& parameters,
                              const fit_settings& settings) {
  TMinuit minuit;
  minuit.SetPrintLevel(-1);
  minuit.SetErrorDef(settings.error_def);
  minuit.SetMaxIterations(settings.max_iterations);

  auto& t0 = parameters.t0;
  minuit.DefineParameter(0, "T0", t0.value, t0.error, t0.min, t0.max);
  auto& x0 = parameters.x0;
  minuit.DefineParameter(1, "X0", x0.value, x0.error, x0.min, x0.max);
  auto& y0 = parameters.y0;
  minuit.DefineParameter(2, "Y0", y0.value, y0.error, y0.min, y0.max);
  auto& z0 = parameters.z0;
  minuit.DefineParameter(3, "Z0", z0.value, z0.error, z0.min, z0.max);
  auto& vx = parameters.vx;
  minuit.DefineParameter(4, "VX", vx.value, vx.error, vx.min, vx.max);
  auto& vy = parameters.vy;
  minuit.DefineParameter(5, "VY", vy.value, vy.error, vy.min, vy.max);
  auto& vz = parameters.vz;
  minuit.DefineParameter(6, "VZ", vz.value, vz.error, vz.min, vz.max);

  _nll_fit_event = event;
  minuit.SetFCN(_gaussian_nll);

  Int_t error_flag;
  auto command_parameters = settings.command_parameters;
  minuit.mnexcm(
    settings.command_name.c_str(),
    command_parameters.data(),
    command_parameters.size(),
    error_flag);

  // std::cout << "\n";

  // TODO: read out error_flag

  Double_t value, error;

  minuit.GetParameter(0, value, error);
  t0.value = value;
  t0.error = error;
  minuit.GetParameter(1, value, error);
  x0.value = value;
  x0.error = error;
  minuit.GetParameter(2, value, error);
  y0.value = value;
  y0.error = error;
  minuit.GetParameter(3, value, error);
  z0.value = value;
  z0.error = error;
  minuit.GetParameter(4, value, error);
  vx.value = value;
  vx.error = error;
  minuit.GetParameter(5, value, error);
  vy.value = value;
  vy.error = error;
  minuit.GetParameter(6, value, error);
  vz.value = value;
  vz.error = error;

  return parameters;
}
//----------------------------------------------------------------------------------------------

//__4x4 Matrix Inverse__________________________________________________________________________
real_array<16> _4x4_inverse(const real_array<16> m) {
  using namespace util::math;

  real det;
  real_array<16> out;

  const auto&& m_04_09 = m[4]  * m[9];
  const auto&& m_04_13 = m[4]  * m[13];
  const auto&& m_05_08 = m[5]  * m[8];
  const auto&& m_05_12 = m[5]  * m[12];
  const auto&& m_06_11 = m[6]  * m[11];
  const auto&& m_06_15 = m[6]  * m[15];
  const auto&& m_07_10 = m[7]  * m[10];
  const auto&& m_07_14 = m[7]  * m[14];
  const auto&& m_08_13 = m[8]  * m[13];
  const auto&& m_09_12 = m[9]  * m[12];
  const auto&& m_10_15 = m[10] * m[15];
  const auto&& m_11_14 = m[11] * m[14];

  out[0]  = fused_product(m[5], m_10_15 - m_11_14, m[9],  m_07_14 - m_06_15, m[13], m_06_11 - m_07_10);
  out[4]  = fused_product(m[4], m_11_14 - m_10_15, m[8],  m_06_15 - m_07_14, m[12], m_07_10 - m_06_11);
  out[8]  = fused_product(m[7], m_08_13 - m_09_12, m[11], m_05_12 - m_04_13, m[15], m_04_09 - m_05_08);
  out[12] = fused_product(m[6], m_09_12 - m_08_13, m[10], m_04_13 - m_05_12, m[14], m_05_08 - m_04_09);

  det = m[0] * out[0] + m[1] * out[4] + m[2] * out[8] + m[3] * out[12];
  if (det == 0) return {};

  const auto&& m_00_05 = m[0] * m[5];
  const auto&& m_00_09 = m[0] * m[9];
  const auto&& m_00_13 = m[0] * m[13];
  const auto&& m_01_04 = m[1] * m[4];
  const auto&& m_01_08 = m[1] * m[8];
  const auto&& m_01_12 = m[1] * m[12];
  const auto&& m_02_07 = m[2] * m[7];
  const auto&& m_02_11 = m[2] * m[11];
  const auto&& m_02_15 = m[2] * m[15];
  const auto&& m_03_06 = m[3] * m[6];
  const auto&& m_03_10 = m[3] * m[10];
  const auto&& m_03_14 = m[3] * m[14];
  const auto&& m_05_10 = m[5] * m[10];

  out[1]  = fused_product(m[1],  m_11_14 - m_10_15, m[9],  m_02_15 - m_03_14, m[13], m_03_10 - m_02_11);
  out[5]  = fused_product(m[0],  m_10_15 - m_11_14, m[8],  m_03_14 - m_02_15, m[12], m_02_11 - m_03_10);
  out[2]  = fused_product(m[1],  m_06_15 - m_07_14, m[5],  m_03_14 - m_02_15, m[13], m_02_07 - m_03_06);
  out[6]  = fused_product(m[0],  m_07_14 - m_06_15, m[4],  m_02_15 - m_03_14, m[12], m_03_06 - m_02_07);
  out[3]  = fused_product(m[1],  m_07_10 - m_06_11, m[5],  m_02_11 - m_03_10, m[9],  m_03_06 - m_02_07);
  out[7]  = fused_product(m[0],  m_06_11 - m_07_10, m[4],  m_03_10 - m_02_11, m[8],  m_02_07 - m_03_06);
  out[9]  = fused_product(m[3],  m_09_12 - m_08_13, m[11], m_00_13 - m_01_12, m[15], m_01_08 - m_00_09);
  out[13] = fused_product(m[2],  m_08_13 - m_09_12, m[10], m_01_12 - m_00_13, m[14], m_00_09 - m_01_08);
  out[10] = fused_product(m[3],  m_04_13 - m_05_12, m[7],  m_01_12 - m_00_13, m[15], m_00_05 - m_01_04);
  out[14] = fused_product(m[2],  m_05_12 - m_04_13, m[6],  m_00_13 - m_01_12, m[14], m_01_04 - m_00_05);
  out[11] = fused_product(m[3],  m_05_08 - m_04_09, m[7],  m_00_09 - m_01_08, m[11], m_01_04 - m_00_05);
  out[15] = fused_product(m[2],  m_04_09 - m_05_08, m[6],  m_01_08 - m_00_09, m[10], m_00_05 - m_01_04);

  const auto&& inv_det = 1.0L / det;
  std::transform(out.cbegin(), out.cend(), out.begin(), [&](const auto& value) { return value * inv_det; });
  return out;
}
//----------------------------------------------------------------------------------------------

} /* anonymous namespace */ ////////////////////////////////////////////////////////////////////

//__Track Constructor___________________________________________________________________________
track::track(const event_points& event, const fit_settings& settings)
    : _event(event), _settings(settings) {

  auto fit_track = _guess_track(_event);
  _fit_event(_event, fit_track, _settings);

  /*

  Track Parameters:
  T0: 335.253  (+/- 2)
  X0: 274.849  (+/- 1000)
  Y0: 688.753  (+/- 1000)
  Z0: -499  (+/- 1000)
  VX: 0.48139  (+/- 29.9792)
  VY: 8.20171  (+/- 29.9792)
  VZ: -29.9602  (+/- 29.9792)


  Track Parameters:
  T0: 335.253  (+/- 1)
  X0: 274.849  (+/- 1)
  Y0: 688.753  (+/- 1)
  Z0: -499  (+/- 1)
  VX: 0.48139  (+/- 68.6857)
  VY: 8.20171  (+/- 65.1366)
  VZ: 0.000285904  (+/- 68.9068)

   */


  _t0 = std::move(fit_track.t0);
  _x0 = std::move(fit_track.x0);
  _y0 = std::move(fit_track.y0);
  _z0 = std::move(fit_track.z0);
  _vx = std::move(fit_track.vx);
  _vy = std::move(fit_track.vy);
  _vz = std::move(fit_track.vz);

  const auto& event_begin = _event.cbegin();
  const auto& event_end = _event.cend();

  const auto& covariance = covariance_matrix(_event);
  const auto& row0 = covariance[0];
  const auto& row1 = covariance[1];
  const auto& row2 = covariance[2];
  const auto& row3 = covariance[3];
  const auto& inverse_covariance = _4x4_inverse({
    row0.t, row0.x, row0.y, row0.z,
    row1.t, row1.x, row1.y, row1.z,
    row2.t, row2.x, row2.y, row2.z,
    row3.t, row3.x, row3.y, row3.z});

  std::transform(event_begin, event_end, std::back_inserter(_delta_chi_squared),
    [&](const auto& point) {
      const auto& delta = point - (*this)(point.z);
      const real_array<4> delta_array{delta.t, delta.x, delta.y, delta.z};
      real chi_sq;
      for (size_t i = 0; i < 4; ++i) { for (size_t j = 0; j < 4; ++j) {
        chi_sq += inverse_covariance[4*i+j] * delta_array[i] * delta_array[j];
      } }
      return chi_sq;
    });

  std::transform(event_begin, event_end, std::back_inserter(_detectors),
    [&](const auto& point) { return geometry::volume(point); });

  std::transform(_detectors.cbegin(), _detectors.cend(), std::back_inserter(_squared_residuals),
    [&](const auto& detector) {
      return _track_squared_residual(
        _t0.value,
        _x0.value,
        _y0.value,
        _z0.value,
        _vx.value,
        _vy.value,
        _vz.value,
        detector);
    });
}
//----------------------------------------------------------------------------------------------

//__Get Position of Track at Fixed Z____________________________________________________________
const r4_point track::operator()(const real z) const {
  const auto&& dt = (z - _z0.value) / _vz.value;
  return { dt + _t0.value, std::fma(dt, _vx.value, _x0.value), std::fma(dt, _vy.value, _y0.value), z };
}
//----------------------------------------------------------------------------------------------

//__Total Residual______________________________________________________________________________
real track::residual() const {
  return std::sqrt(squared_residual());
}
//----------------------------------------------------------------------------------------------

//__Total Squared-Residual______________________________________________________________________
real track::squared_residual() const {
  return std::accumulate(_squared_residuals.cbegin(), _squared_residuals.cend(), 0);
}
//----------------------------------------------------------------------------------------------

//__Residual at Each Volume_____________________________________________________________________
const real_vector track::residual_vector() const {
  real_vector out;
  out.reserve(_squared_residuals.size());
  std::transform(_squared_residuals.cbegin(), _squared_residuals.cend(), std::back_inserter(out),
    [&](const auto& sq_res) { return std::sqrt(sq_res); });
  return out;
}
//----------------------------------------------------------------------------------------------

//__Relativistic Beta for the Track_____________________________________________________________
real track::beta() const {
  return std::sqrt(util::math::fused_product(
    _vx.value, _vx.value, _vy.value, _vy.value, _vz.value, _vz.value)) / units::speed_of_light;
}
//----------------------------------------------------------------------------------------------

//__Chi-Squared Test Statistic__________________________________________________________________
real track::chi_squared() const {
  return std::accumulate(_delta_chi_squared.cbegin(), _delta_chi_squared.cend(), 0);
}
//----------------------------------------------------------------------------------------------

//__Track Degree of Freedom_____________________________________________________________________
integer track::degree_of_freedom() const {
  return 4 * _event.size() - 6;
}
//----------------------------------------------------------------------------------------------

//__Chi-Squared per Degree of Freedom___________________________________________________________
real track::chi_squared_per_dof() const {
  return chi_squared() / degree_of_freedom();
}
//----------------------------------------------------------------------------------------------

//__Output Stream Operator______________________________________________________________________
std::ostream& operator<<(std::ostream& os, const track& t) {
  return os << "Track Parameters: \n"
    << "  T0: " << t.t0_value() << "  (+/- " << t.t0_error() << ")\n"
    << "  X0: " << t.x0_value() << "  (+/- " << t.x0_error() << ")\n"
    << "  Y0: " << t.y0_value() << "  (+/- " << t.y0_error() << ")\n"
    << "  Z0: " << t.z0_value() << "  (+/- " << t.z0_error() << ")\n"
    << "  VX: " << t.vx_value() << "  (+/- " << t.vx_error() << ")\n"
    << "  VY: " << t.vy_value() << "  (+/- " << t.vy_error() << ")\n"
    << "  VZ: " << t.vz_value() << "  (+/- " << t.vz_error() << ")\n";
}
//----------------------------------------------------------------------------------------------

//__Add Track from Seed to Track Vector_________________________________________________________
track_vector& operator+=(track_vector& tracks, const event_points& seed) {
  if (tracks.empty()) tracks.emplace_back(seed);
  else tracks.emplace_back(seed, tracks.front().settings());
  return tracks;
}
//----------------------------------------------------------------------------------------------

//__Fit all Seeds to Tracks_____________________________________________________________________
track_vector fit_seeds(const event_vector& seeds,
                       const fit_settings& settings) {
  track_vector out;
  out.reserve(seeds.size());
  std::transform(seeds.cbegin(), seeds.cend(), std::back_inserter(out),
    [&](const auto& seed) { return track(seed, settings); });
  return out;
}
//----------------------------------------------------------------------------------------------

} /* namespace analysis */ /////////////////////////////////////////////////////////////////////

} } /* namespace MATHUSLA::TRACKER */
