#include "tile.h"
#include <algorithm>
#include <functional>
#ifdef _HAS_CUDA_
#include "kernels.h"
#endif
#include "cpu_kernels.h"

namespace SCAMP {

// Gets the exclusion zone for a particular tile (helper_function)
static int get_exclusion(uint64_t window_size, int64_t start_row,
                         int64_t start_column) {
  int exclusion = window_size / 4;
  if (start_column >= start_row && start_column < start_row + exclusion) {
    return exclusion;
  }
  return 0;
}

std::pair<int, int> Tile::get_exclusion_for_self_join(bool upper_tile) {
  int exclusion;
  int extra_exclusion = 0;
  if (_info->profile_type == PROFILE_TYPE_SUM_THRESH ||
      _info->profile_type == PROFILE_TYPE_FREQUENCY_THRESH) {
    // We need to omit the main diagonal from one tile so it doesn't get
    // double counted
    extra_exclusion = 1;
  }
  if (upper_tile) {
    exclusion = get_exclusion(_info->mp_window, get_tile_row(), get_tile_col());
    return std::make_pair(exclusion, extra_exclusion);
  }
  size_t height = get_tile_height() - _info->mp_window + 1;
  exclusion =
      get_exclusion(_info->mp_window, get_tile_col(), get_tile_row() + height);
  return std::make_pair(extra_exclusion, exclusion);
}

// Gets the exclusion zone for a particular tile (logic for ab joins)
std::pair<int, int> Tile::get_exclusion_for_ab_join(bool upper_tile) {
  int exclusion_lower = 0;
  int exclusion_upper = 0;

  if (!_info->is_aligned) {
    if (upper_tile && (_info->profile_type == PROFILE_TYPE_SUM_THRESH ||
                       _info->profile_type == PROFILE_TYPE_FREQUENCY_THRESH)) {
      exclusion_lower += 1;
    }
    return std::make_pair(exclusion_lower, exclusion_upper);
  }
  size_t height = get_tile_height() - _info->mp_window + 1;
  size_t width = get_tile_width() - _info->mp_window + 1;

  int start_col = get_tile_col();
  int start_row = get_tile_row();
  if (_info->global_start_col_position >= 0 &&
      _info->global_start_row_position >= 0) {
    start_col += _info->global_start_col_position;
    start_row += _info->global_start_row_position;
  }
  if (upper_tile) {
    exclusion_lower = get_exclusion(_info->mp_window, start_row, start_col);
    if (exclusion_lower == 0 &&
        (_info->profile_type == PROFILE_TYPE_SUM_THRESH ||
         _info->profile_type == PROFILE_TYPE_FREQUENCY_THRESH)) {
      // We need to omit the main diagonal from one tile so it doesn't get
      // double counted
      exclusion_lower += 1;
    }
    if (start_row > start_col) {
      exclusion_upper =
          get_exclusion(_info->mp_window, start_row, start_col + width);
    } else {
      exclusion_upper = 0;
    }
  } else {
    exclusion_lower = get_exclusion(_info->mp_window, start_col, start_row);
    if (start_row >= start_col) {
      exclusion_upper = 0;
    } else {
      exclusion_upper =
          get_exclusion(_info->mp_window, start_col, start_row + height);
    }
  }
  return std::make_pair(exclusion_lower, exclusion_upper);
}

// Allocator for tile memory which can reside on the host or cuda devices
template <typename T>
T *alloc_mem(size_t count, SCAMPArchitecture arch, int deviceid) {
  switch (arch) {
    case CUDA_GPU_WORKER: {
#ifdef _HAS_CUDA_
      cudaSetDevice(deviceid);
      gpuErrchk(cudaPeekAtLastError());
      size_t bytes = count * sizeof(T);
      T *ptr;
      cudaMalloc(&ptr, bytes);
      gpuErrchk(cudaPeekAtLastError());
      return ptr;
#else
      ASSERT(false, "Using CUDA in binary not built with it");
      return nullptr;
#endif
    }
    case CPU_WORKER:
      return new T[count];  // NOLINT
  }
}

// Deleter for tile memory which can reside on the host or cuda devices
template <typename T>
void free_mem(T *ptr, SCAMPArchitecture arch, int deviceid) {
  switch (arch) {
    case CUDA_GPU_WORKER:
#ifdef _HAS_CUDA_
      cudaSetDevice(deviceid);
      gpuErrchk(cudaPeekAtLastError());
      cudaFree(ptr);
      gpuErrchk(cudaPeekAtLastError());
#else
      ASSERT(false, "Using CUDA in binary not built with it");
#endif
      break;
    case CPU_WORKER:
      delete[] ptr;  // NOLINT
      break;
  }
}

void Tile::Memset(void *destination, char value, size_t bytes) {
  switch (get_arch()) {
    case CUDA_GPU_WORKER:
#ifdef _HAS_CUDA_
      cudaSetDevice(get_cuda_id());
      gpuErrchk(cudaPeekAtLastError());
      cudaMemsetAsync(destination, value, bytes, get_stream());
      gpuErrchk(cudaPeekAtLastError());
#else
      ASSERT(false, "Using CUDA in binary not built with it");
#endif
      break;
    case CPU_WORKER:
      memset(destination, value, bytes);
      break;
  }
}

void Tile::Memcopy(void *destination, const void *source, size_t bytes,
                   bool from_tile) {
  SCAMP::Memcopy(destination, source, bytes, from_tile, &_exec_info);
}

Tile::Tile(const OpInfo *info, SCAMPArchitecture arch, int cuda_id)
    : _info(info),
      _exec_info(arch, cuda_id),
      _current_tile_width(0),
      _current_tile_height(0),
      _current_tile_row(0),
      _current_tile_col(0),
      // Allocate memory for tile based on architecture
      _T_A_dev(alloc_mem<double>(info->max_tile_ts_size, arch, cuda_id),
               // Lambda deallocator
               [=](double *p) { return free_mem<double>(p, arch, cuda_id); }),
      _T_B_dev(alloc_mem<double>(info->max_tile_ts_size, arch, cuda_id),
               [=](double *p) { return free_mem<double>(p, arch, cuda_id); }),
      _QT_dev(alloc_mem<double>(info->max_tile_width, arch, cuda_id),
              [=](double *p) { return free_mem<double>(p, arch, cuda_id); }),
      _means_A(alloc_mem<double>(info->max_tile_width, arch, cuda_id),
               [=](double *p) { return free_mem<double>(p, arch, cuda_id); }),
      _means_B(alloc_mem<double>(info->max_tile_height, arch, cuda_id),
               [=](double *p) { return free_mem<double>(p, arch, cuda_id); }),
      _norms_A(alloc_mem<double>(info->max_tile_width, arch, cuda_id),
               [=](double *p) { return free_mem<double>(p, arch, cuda_id); }),
      _norms_B(alloc_mem<double>(info->max_tile_height, arch, cuda_id),
               [=](double *p) { return free_mem<double>(p, arch, cuda_id); }),
      _df_A(alloc_mem<double>(info->max_tile_width, arch, cuda_id),
            [=](double *p) { return free_mem<double>(p, arch, cuda_id); }),
      _df_B(alloc_mem<double>(info->max_tile_height, arch, cuda_id),
            [=](double *p) { return free_mem<double>(p, arch, cuda_id); }),
      _dg_A(alloc_mem<double>(info->max_tile_width, arch, cuda_id),
            [=](double *p) { return free_mem<double>(p, arch, cuda_id); }),
      _dg_B(alloc_mem<double>(info->max_tile_height, arch, cuda_id),
            [=](double *p) { return free_mem<double>(p, arch, cuda_id); }),

      _scratchpad(
          static_cast<double *>(
              alloc_mem<double>(info->max_tile_ts_size, arch, cuda_id)),
          [=](double *p) { return free_mem<double>(p, arch, cuda_id); }),

      _scratch(
          std::unique_ptr<qt_compute_helper>(new qt_compute_helper(  // NOLINT
              info->max_tile_ts_size, info->mp_window, true, arch))),
      _profile_a_tile(info->profile_type, info->max_tile_width),
      _profile_b_tile(info->profile_type, info->max_tile_height) {
  size_t profile_size = GetProfileTypeSize(_info->profile_type);
  size_t rows_to_alloc, cols_to_alloc;

  // For profile types where we can have more than one match per tile we need
  // to allocate additional memory
  if (_info->profile_type == PROFILE_TYPE_APPROX_ALL_NEIGHBORS) {
    cols_to_alloc = _info->max_matches_per_tile;
    rows_to_alloc = _info->max_matches_per_tile;
  } else {
    cols_to_alloc = _info->max_tile_width;
    rows_to_alloc = _info->max_tile_height;
  }

  // Allocate the tile's device memory
  _profile_a_tile_dev[_info->profile_type] =
      alloc_mem<char>(profile_size * cols_to_alloc, arch, cuda_id);
  _profile_b_tile_dev[_info->profile_type] =
      alloc_mem<char>(profile_size * rows_to_alloc, arch, cuda_id);

  // Allocate variable to track number of outputs generated by the kernel
  _profile_a_dev_length = alloc_mem<unsigned long long int>(1, arch, cuda_id);
  _profile_b_dev_length = alloc_mem<unsigned long long int>(1, arch, cuda_id);
}

Tile::~Tile() {
  // Free any memory allocated that will not be freed automatically
  free_mem<char>(static_cast<char *>(_profile_a_tile_dev[_info->profile_type]),
                 get_arch(), get_cuda_id());
  free_mem<char>(static_cast<char *>(_profile_b_tile_dev[_info->profile_type]),
                 get_arch(), get_cuda_id());
  free_mem<unsigned long long int>(_profile_a_dev_length, get_arch(),
                                   get_cuda_id());
  free_mem<unsigned long long int>(_profile_b_dev_length, get_arch(),
                                   get_cuda_id());
}

void Tile::Sync() {
  switch (get_arch()) {
    case CUDA_GPU_WORKER:
#if _HAS_CUDA_
      cudaStreamSynchronize(get_stream());
      gpuErrchk(cudaPeekAtLastError());
#else
      ASSERT(false, "ERROR: CUDA used in binary not built with CUDA");
#endif
      break;
    case CPU_WORKER:
      break;
  }
}

void Tile::InitTimeseries(const std::vector<double> &Ta_h,
                          const std::vector<double> &Tb_h) {
  Memcopy(_T_A_dev.get(), Ta_h.data() + _current_tile_col,
          sizeof(double) * _current_tile_width, false);
  Memcopy(_T_B_dev.get(), Tb_h.data() + _current_tile_row,
          sizeof(double) * _current_tile_height, false);
}

// Initializes the tile's local profile values based on global profiles
// "profile_a" and "profile_b"
SCAMPError_t Tile::InitProfile(Profile *profile_a, Profile *profile_b) {
  int profile_size = GetProfileTypeSize(_info->profile_type);
  int width = _current_tile_width - _info->mp_window + 1;
  int height = _current_tile_height - _info->mp_window + 1;
  SCAMPProfileType type = _info->profile_type;
  switch (type) {
    case PROFILE_TYPE_SUM_THRESH:
      Memset(_profile_a_tile_dev.at(type), 0, profile_size * width);
      Memset(_profile_b_tile_dev.at(type), 0, profile_size * height);
      break;
    case PROFILE_TYPE_1NN_INDEX: {
      const uint64_t *pA_ptr = profile_a->data[0].uint64_value.data();
      Memcopy(_profile_a_tile_dev.at(type), pA_ptr + _current_tile_col,
              sizeof(uint64_t) * width, false);
      if (_info->computing_rows && _info->keep_rows_separate) {
        const uint64_t *pB_ptr = profile_b->data[0].uint64_value.data();
        Memcopy(_profile_b_tile_dev.at(type), pB_ptr + _current_tile_row,
                sizeof(uint64_t) * height, false);
      } else if (_info->self_join) {
        Memcopy(_profile_b_tile_dev.at(type), pA_ptr + _current_tile_row,
                sizeof(uint64_t) * height, false);
      }
      break;
    }
    case PROFILE_TYPE_1NN: {
      const float *pA_ptr = profile_a->data[0].float_value.data();
      Memcopy(_profile_a_tile_dev.at(type), pA_ptr + _current_tile_col,
              sizeof(float) * width, false);
      if (_info->self_join) {
        Memcopy(_profile_b_tile_dev.at(type), pA_ptr + _current_tile_row,
                sizeof(float) * height, false);
      } else if (_info->computing_rows && _info->keep_rows_separate) {
        const float *pB_ptr = profile_b->data[0].float_value.data();
        Memcopy(_profile_b_tile_dev.at(type), pB_ptr + _current_tile_row,
                sizeof(float) * height, false);
      }
      break;
    }
    case PROFILE_TYPE_APPROX_ALL_NEIGHBORS:
      Memset(_profile_a_dev_length, 0, sizeof(unsigned long long int));
      Memset(_profile_b_dev_length, 0, sizeof(unsigned long long int));
      break;
    case PROFILE_TYPE_FREQUENCY_THRESH:
    case PROFILE_TYPE_KNN:
    case PROFILE_TYPE_1NN_MULTIDIM:
    case PROFILE_TYPE_INVALID:
      return SCAMP_FUNCTIONALITY_UNIMPLEMENTED;
  }
  return SCAMP_NO_ERROR;
}

// Initialize the tile's local stats based on global statistics "a" and "b"
void Tile::InitStats(const PrecomputedInfo &a, const PrecomputedInfo &b) {
  size_t bytes_a =
      (_current_tile_width - _info->mp_window + 1) * sizeof(double);
  size_t bytes_b =
      (_current_tile_height - _info->mp_window + 1) * sizeof(double);
  Memcopy(_norms_A.get(), a.norms().data() + _current_tile_col, bytes_a, false);
  Memcopy(_df_A.get(), a.df().data() + _current_tile_col, bytes_a, false);
  Memcopy(_dg_A.get(), a.dg().data() + _current_tile_col, bytes_a, false);
  Memcopy(_means_A.get(), a.means().data() + _current_tile_col, bytes_a, false);
  Memcopy(_norms_B.get(), b.norms().data() + _current_tile_row, bytes_b, false);
  Memcopy(_df_B.get(), b.df().data() + _current_tile_row, bytes_b, false);
  Memcopy(_dg_B.get(), b.dg().data() + _current_tile_row, bytes_b, false);
  Memcopy(_means_B.get(), b.means().data() + _current_tile_row, bytes_b, false);
}

std::pair<unsigned long long int, unsigned long long int>
Tile::get_profile_dims_from_device() {
  std::pair<unsigned long long int, unsigned long long int> result;
  result.first = 0;
  result.second = 0;
  this->Memcopy(&result.first, _profile_a_dev_length,
                sizeof(unsigned long long int), true);
  this->Memcopy(&result.second, _profile_b_dev_length,
                sizeof(unsigned long long int), true);
  Sync();
  if (result.first > info()->max_matches_per_tile) {
    if (!_info->silent_mode) {
      std::cout << "Warning: Unable to return all matches! SCAMP found a "
                   "total of "
                << result.first
                << " matches for this tile. But we could only store "
                << _info->max_matches_per_tile
                << " of them. Perhaps try a smaller tile size or a higher "
                   "match threshold? "
                << std::endl;
    }
    result.first = _info->max_matches_per_tile;
  }

  if (result.second > info()->max_matches_per_tile) {
    if (!_info->silent_mode) {
      std::cout << "Warning: Unable to return all matches! SCAMP found a "
                   "total of "
                << result.second
                << " matches for this tile. But we could only store "
                << _info->max_matches_per_tile
                << " of them. Perhaps try a smaller tile size or a higher "
                   "match threshold? "
                << std::endl;
    }
    result.second = _info->max_matches_per_tile;
  }
  if (!_info->silent_mode) {
    std::cout << "width = " << result.first << " height = " << result.second
              << std::endl;
  }
  return result;
}

// TODO(zpzim): move this back into SCAMP_Operation, we shouldn't have the
// merging be functionality of the individual tile
void Tile::MergeProfile(Profile *profile_a, Profile *profile_b) {
  // Set up a copy operation back to the host
  int height, width;
  switch (profile_a->type) {
    case PROFILE_TYPE_1NN:
    case PROFILE_TYPE_1NN_INDEX:
    case PROFILE_TYPE_SUM_THRESH:
      // We already know how many elements to copy back
      width = _current_tile_width - _info->mp_window + 1;
      height = _current_tile_height - _info->mp_window + 1;
      break;
    case PROFILE_TYPE_APPROX_ALL_NEIGHBORS: {
      // We need to find the number of elements generated by the kernel
      auto width_height = get_profile_dims_from_device();
      width = width_height.first;
      height = width_height.second;
      break;
    }
    default:
      throw(SCAMPException("Functionality Unimplemented."));
      break;
  }

  _profile_a_tile.CopyFromDevice(&_exec_info, &_profile_a_tile_dev, width);
  if (_info->computing_rows) {
    _profile_b_tile.CopyFromDevice(&_exec_info, &_profile_b_tile_dev, height);
  }

  // Wait for the previous work to be done
  Sync();

  // Merge result
  profile_a->MergeTileToProfile(&_profile_a_tile, _info, _current_tile_col,
                                width, _current_tile_row);

  if (_info->computing_rows && _info->keep_rows_separate) {
    profile_b->MergeTileToProfile(&_profile_b_tile, _info, _current_tile_row,
                                  height, _current_tile_col);
  } else if (_info->self_join) {
    profile_a->MergeTileToProfile(&_profile_b_tile, _info, _current_tile_row,
                                  height, _current_tile_col);
  }
}

SCAMPError_t Tile::execute(SCAMPTileType t) {
  SCAMPError_t error;
  switch (t) {
    case SELF_JOIN_FULL_TILE:
      error = do_self_join_full();
      break;
    case SELF_JOIN_UPPER_TRIANGULAR:
      error = do_self_join_half();
      break;
    case AB_JOIN_FULL_TILE:
      error = do_ab_join_full();
      break;
    case AB_FULL_JOIN_FULL_TILE:
      error = do_ab_join_full();
      break;
    default:
      error = SCAMP_TILE_ILLEGAL_TYPE;
      break;
  }
  return error;
}

SCAMPError_t Tile::do_self_join_full() {
  SCAMPError_t error = SCAMP_NO_ERROR;

  // Compute the upper triangular portion of the tile
  error = do_self_join_half();
  if (error != SCAMP_NO_ERROR) {
    return error;
  }

  // Compute the lower triangular portion of the tile based on worker arch
  switch (get_arch()) {
    case CUDA_GPU_WORKER:
#ifdef _HAS_CUDA_
      error =
          _scratch->compute_QT(_QT_dev.get(), _T_B_dev.get(), _T_A_dev.get(),
                               _means_A.get(), get_stream());
      if (error != SCAMP_NO_ERROR) {
        return error;
      }
      error = gpu_kernel_self_join_lower(this);
#else
      ASSERT(false, "ERROR: CUDA used in binary not built with CUDA");
#endif
      break;
    case CPU_WORKER:
      error = _scratch->compute_QT_CPU(_QT_dev.get(), _T_B_dev.get(),
                                       _T_A_dev.get());
      if (error != SCAMP_NO_ERROR) {
        return error;
      }
      error = cpu_kernel_self_join_lower(this);
      break;
  }
  return error;
}

// Computes the matrix profile upper triangular portion of the tile
SCAMPError_t Tile::do_self_join_half() {
  SCAMPError_t error = SCAMP_NO_ERROR;

  if (_info->mp_window > _current_tile_width) {
    return SCAMP_DIM_INCOMPATIBLE;
  }
  if (_info->mp_window > _current_tile_height) {
    return SCAMP_DIM_INCOMPATIBLE;
  }

  // Compute the upper triangular portion of the tile based on worker arch
  switch (get_arch()) {
    case CUDA_GPU_WORKER:
#ifdef _HAS_CUDA_
      error =
          _scratch->compute_QT(_QT_dev.get(), _T_A_dev.get(), _T_B_dev.get(),
                               _means_B.get(), get_stream());
      if (error != SCAMP_NO_ERROR) {
        return error;
      }
      error = gpu_kernel_self_join_upper(this);
#else
      ASSERT(false, "ERROR: CUDA used in binary not built with CUDA");
#endif
      break;
    case CPU_WORKER:
      error = _scratch->compute_QT_CPU(_QT_dev.get(), _T_A_dev.get(),
                                       _T_B_dev.get());
      if (error != SCAMP_NO_ERROR) {
        return error;
      }
      error = cpu_kernel_self_join_upper(this);
      break;
  }
  if (error != SCAMP_NO_ERROR) {
    return error;
  }
  return SCAMP_NO_ERROR;
}

SCAMPError_t Tile::do_ab_join_full() {
  SCAMPError_t error = SCAMP_NO_ERROR;

  if (_info->mp_window > _current_tile_width) {
    return SCAMP_DIM_INCOMPATIBLE;
  }
  if (_info->mp_window > _current_tile_height) {
    return SCAMP_DIM_INCOMPATIBLE;
  }

  switch (get_arch()) {
    case CUDA_GPU_WORKER:
#ifdef _HAS_CUDA_
      error =
          _scratch->compute_QT(_QT_dev.get(), _T_A_dev.get(), _T_B_dev.get(),
                               _means_B.get(), get_stream());
      if (error != SCAMP_NO_ERROR) {
        return error;
      }
      error = gpu_kernel_ab_join_upper(this);
#else
      ASSERT(false, "ERROR: CUDA used in binary not built with CUDA");
#endif
      break;
    case CPU_WORKER:
      error = _scratch->compute_QT_CPU(_QT_dev.get(), _T_A_dev.get(),
                                       _T_B_dev.get());
      if (error != SCAMP_NO_ERROR) {
        return error;
      }
      error = cpu_kernel_ab_join_upper(this);
      break;
  }
  if (error != SCAMP_NO_ERROR) {
    return error;
  }

  switch (get_arch()) {
    case CUDA_GPU_WORKER:
#ifdef _HAS_CUDA_
      error =
          _scratch->compute_QT(_QT_dev.get(), _T_B_dev.get(), _T_A_dev.get(),
                               _means_A.get(), get_stream());
      if (error != SCAMP_NO_ERROR) {
        return error;
      }

      error = gpu_kernel_ab_join_lower(this);
#else
      ASSERT(false, "ERROR: CUDA used in binary not built with CUDA");
#endif
      break;
    case CPU_WORKER:
      error = _scratch->compute_QT_CPU(_QT_dev.get(), _T_B_dev.get(),
                                       _T_A_dev.get());
      if (error != SCAMP_NO_ERROR) {
        return error;
      }
      error = cpu_kernel_ab_join_lower(this);
      break;
  }
  return error;
}

}  // namespace SCAMP
