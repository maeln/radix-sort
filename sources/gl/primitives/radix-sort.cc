/*
Copyright (c) 2016, Oleg Ageev
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
Based on work of Takahiro HARADA
https://github.com/takahiroharada/OCLRadixSort
and changes in AMD Bolt C++ Template Library
https://github.com/HSA-Libraries/Bolt
*/

#include "parallel/gl/primitives/radix-sort.hh"

#include "parallel/gl/opengl.hh"

#undef min
#undef max

#define CONSTS 0

#define HISTOGRAM 0
#define DATA 1
#define KEY_IN 0
#define KEY_OUT 1
#define VALUE_IN 2
#define VALUE_OUT 3

#define WG_COUNT 64
#define WG_SIZE 256
#define BLOCK_SIZE 1024  // (4 * WG_SIZE)
#define BITS_PER_PASS 4
#define RADICES 16       // (1 << BITS_PER_PASS)
#define RADICES_MASK 0xf // (RADICES - 1)

static GLchar const * prolog = GLSL(
layout(local_size_x = WG_SIZE) in;
layout(binding = CONSTS) uniform Consts {
  uint shift;
  bool descending;
  bool is_signed;
  bool key_index;
};
layout(binding = HISTOGRAM) buffer Histogram { uint histogram[]; };
layout(binding = DATA) buffer Data { uint buf[]; } data[4];
)
GLSL_DEFINE(EACH(i, size), for (int i = 0; i < size; i++))
GLSL_DEFINE(TO_MASK(n), ((1 << (n)) - 1))
GLSL_DEFINE(BFE(src, s, n), ((src >> s) & TO_MASK(n)))
GLSL_DEFINE(BFE_SIGN(src, s, n),
  (((((src >> s) & TO_MASK(n - 1))
                 ^ TO_MASK(n - 1))
                 & TO_MASK(n - 1))
   | ((src >> s) &  (1 << (n - 1)))))
GLSL_DEFINE(BARRIER, groupMemoryBarrier(); barrier())
GLSL_DEFINE(LC_IDX, gl_LocalInvocationIndex)
GLSL_DEFINE(WG_IDX, gl_WorkGroupID.x)
GLSL_DEFINE(MIX(T, x, y, a), (x) * T(a) + (y) * (1 - T(a)))
GLSL_DEFINE(GET_BY4(T, src, idx), T(src[idx.x], src[idx.y], src[idx.z], src[idx.w]))
GLSL_DEFINE(SET_BY4(dest, idx, val), do {
  dest[idx.x] = val.x;
  dest[idx.y] = val.y;
  dest[idx.z] = val.z;
  dest[idx.w] = val.w;
} while(false))
GLSL_DEFINE(SET_BY4_CHECKED(dest, idx, val, flag), do {
  if (flag.x) dest[idx.x] = val.x;
  if (flag.y) dest[idx.y] = val.y;
  if (flag.z) dest[idx.z] = val.z;
  if (flag.w) dest[idx.w] = val.w;
} while(false))
GLSL_DEFINE(INC_BY4_CHECKED(dest, idx, flag), do {
  atomicAdd(dest[idx.x], uint(flag.x));
  atomicAdd(dest[idx.y], uint(flag.y));
  atomicAdd(dest[idx.z], uint(flag.z));
  atomicAdd(dest[idx.w], uint(flag.w));
} while(false))
GLSL(
struct blocks_info { uint count; uint offset; };
blocks_info get_blocks_info(const uint n, const uint wg_idx) {
  const uint aligned = n + BLOCK_SIZE - (n % BLOCK_SIZE);
  const uint blocks = (n + BLOCK_SIZE - 1) / BLOCK_SIZE;
  const uint blocks_per_wg = (blocks + WG_COUNT - 1) / WG_COUNT;
  const int n_blocks = int(aligned / BLOCK_SIZE) - int(blocks_per_wg * wg_idx);
  return blocks_info(uint(clamp(n_blocks, 0, int(blocks_per_wg))), blocks_per_wg * BLOCK_SIZE * wg_idx);
}
shared uint local_sort[BLOCK_SIZE];
uint prefix_sum(uint data, inout uint total_sum) {
  const uint lc_idx = LC_IDX + WG_SIZE;
  local_sort[LC_IDX] = 0;
  local_sort[lc_idx] = data;
  BARRIER;
  uint tmp;
  for (uint i = 1; i < WG_SIZE; i <<= 1) {
    tmp = local_sort[lc_idx - i];
    BARRIER;
    local_sort[lc_idx] += tmp;
    BARRIER;
  }
  total_sum = local_sort[WG_SIZE * 2 - 1];
  return local_sort[lc_idx - 1];
}
uint prefix_scan(inout uvec4 v) {
  uint sum = 0;
  uint tmp;
  tmp = v.x; v.x = sum; sum += tmp;
  tmp = v.y; v.y = sum; sum += tmp;
  tmp = v.z; v.z = sum; sum += tmp;
  tmp = v.w; v.w = sum; sum += tmp;
  return sum;
});

static GLchar const * histogram_count = GLSL(
shared uint local_histogram[WG_SIZE * RADICES];
void main() {
  EACH(i, RADICES) local_histogram[i * WG_SIZE + LC_IDX] = 0;
  BARRIER;

  const uint n = data[KEY_IN].buf.length();
  const blocks_info blocks = get_blocks_info(n, WG_IDX);
  uvec4 addr = blocks.offset + 4 * LC_IDX + uvec4(0, 1, 2, 3);
  EACH(i_block, blocks.count) {
    const bvec4 less_than = lessThan(addr, uvec4(n));
    const uvec4 data_vec = GET_BY4(uvec4, data[KEY_IN].buf, addr);
    const uvec4 k = is_signed
      ? BFE_SIGN(data_vec, shift, BITS_PER_PASS)
      : BFE(data_vec, shift, BITS_PER_PASS);
    const uvec4 key = descending != is_signed ? (RADICES_MASK - k) : k;
    const uvec4 local_key = key * WG_SIZE + LC_IDX;
    INC_BY4_CHECKED(local_histogram, local_key, less_than);
    addr += BLOCK_SIZE;
  }
  BARRIER;

  if (LC_IDX < RADICES) {
    uint sum = 0; EACH(i, WG_SIZE) sum += local_histogram[LC_IDX * WG_SIZE + i];
    histogram[LC_IDX * WG_COUNT + WG_IDX] = sum;
  }
  BARRIER;
});

static GLchar const * prefix_scan = GLSL(
shared uint seed;
void main() {
  seed = 0;
  BARRIER;

  EACH(d, RADICES) {
    uint val = 0;
    uint idx = d * WG_COUNT + LC_IDX;
    if (LC_IDX < WG_COUNT) val = histogram[idx];
    uint total;
    uint res = prefix_sum(val, total);
    if (LC_IDX < WG_COUNT) histogram[idx] = res + seed;
    if (LC_IDX == WG_COUNT - 1) seed += res + val;
    BARRIER;
  }
});

static GLchar const * permute = GLSL(
shared uint local_sort_val[BLOCK_SIZE];
void sort_bits(inout uvec4 sort, inout uvec4 sort_val) {
  uvec4 signs = BFE_SIGN(sort, shift, BITS_PER_PASS);
  const uvec4 addr = 4 * LC_IDX + uvec4(0, 1, 2, 3);
  EACH(i_bit, BITS_PER_PASS) {
    const uint mask = (1 << i_bit);
    const bvec4 cmp = equal((is_signed ? signs : (sort >> shift)) & mask, uvec4(descending != is_signed) * mask);
    uvec4 key = uvec4(cmp);
    uint total;
    key += prefix_sum(prefix_scan(key), total);
    BARRIER;

    const uvec4 dest_addr = MIX(uvec4, key, addr - key + total, cmp);
    SET_BY4(local_sort, dest_addr, sort);
    SET_BY4(local_sort_val, dest_addr, sort_val);
    BARRIER;

    sort = GET_BY4(uvec4, local_sort, addr);
    sort_val = GET_BY4(uvec4, local_sort_val, addr);
    BARRIER;

    if (is_signed) {
      SET_BY4(local_sort, dest_addr, signs);
      BARRIER;

      signs = GET_BY4(uvec4, local_sort, addr);
      BARRIER;
    }
  }
}
shared uint local_histogram_to_carry[RADICES];
shared uint local_histogram[RADICES * 2];
void main() {
  const uint carry_idx = (descending && !is_signed ? (RADICES_MASK - LC_IDX) : LC_IDX);
  if (LC_IDX < RADICES) local_histogram_to_carry[LC_IDX] = histogram[carry_idx * WG_COUNT + WG_IDX];
  BARRIER;

  const uint def = (uint(!descending) * 0xffffffff) ^ (uint(is_signed) * 0x80000000);
  const uint n = data[KEY_IN].buf.length();
  const blocks_info blocks = get_blocks_info(n, WG_IDX);
  uvec4 addr = blocks.offset + 4 * LC_IDX + uvec4(0, 1, 2, 3);
  EACH(i_block, blocks.count) {
    const bvec4 less_than = lessThan(addr, uvec4(n));
    const bvec4 less_than_val = lessThan(addr, uvec4(key_index ? n : 0));
    const uvec4 data_vec = GET_BY4(uvec4, data[KEY_IN].buf, addr);
    const uvec4 data_val_vec = GET_BY4(uvec4, data[VALUE_IN].buf, addr);
    uvec4 sort = MIX(uvec4, data_vec, def, less_than);
    uvec4 sort_val = MIX(uvec4, data_val_vec, 0, less_than_val);
    sort_bits(sort, sort_val);
    uvec4 k = is_signed
      ? BFE_SIGN(sort, shift, BITS_PER_PASS)
      : BFE(sort, shift, BITS_PER_PASS);
    const uvec4 key = (descending != is_signed) ? (RADICES_MASK - k) : k;
    const uvec4 hist_key = key + RADICES;
    const uvec4 local_key = key + (LC_IDX / RADICES) * RADICES;
    k = is_signed ? key : k;
    const uvec4 offset = GET_BY4(uvec4, local_histogram_to_carry, k) + 4 * LC_IDX + uvec4(0, 1, 2, 3);
    local_sort[LC_IDX] = 0;
    BARRIER;

    INC_BY4_CHECKED(local_sort, local_key, less_than);
    BARRIER;

    const uint lc_idx = LC_IDX + RADICES;
    if (LC_IDX < RADICES) {
      local_histogram[LC_IDX] = 0;
      uint sum = 0; EACH(i, WG_SIZE / RADICES) sum += local_sort[i * RADICES + LC_IDX];
      local_histogram_to_carry[carry_idx] += local_histogram[lc_idx] = sum;
    }
    BARRIER;

    if (LC_IDX < RADICES) {
      local_histogram[lc_idx] = local_histogram[lc_idx - 1];
      atomicAdd(local_histogram[lc_idx],
        local_histogram[lc_idx - 3] +
        local_histogram[lc_idx - 2] +
        local_histogram[lc_idx - 1]);
      atomicAdd(local_histogram[lc_idx],
        local_histogram[lc_idx - 12] +
        local_histogram[lc_idx - 8] +
        local_histogram[lc_idx - 4]);
    }
    BARRIER;

    const uvec4 out_key = offset - GET_BY4(uvec4, local_histogram, hist_key);
    SET_BY4_CHECKED(data[KEY_OUT].buf, out_key, sort, less_than);
    SET_BY4_CHECKED(data[VALUE_OUT].buf, out_key, sort_val, less_than_val);
    BARRIER;
    addr += BLOCK_SIZE;
  }
});

static GLchar const * flip_float = GLSL(
void main() {
  const uint n = data[KEY_IN].buf.length();
  const blocks_info blocks = get_blocks_info(n, WG_IDX);
  uvec4 addr = blocks.offset + 4 * LC_IDX + uvec4(0, 1, 2, 3);
  EACH(i_block, blocks.count) {
    const bvec4 less_than = lessThan(addr, uvec4(n));
    const uvec4 data_vec = GET_BY4(uvec4, data[KEY_IN].buf, addr);
    uvec4 value = MIX(uvec4, data_vec, 0, less_than);
    uvec4 mask = is_signed ? ((value >> 31) - 1) | 0x80000000 : -ivec4(value >> 31) | 0x80000000;
    value ^= mask;
    SET_BY4_CHECKED(data[KEY_IN].buf, addr, value, less_than);
    addr += BLOCK_SIZE;
  }
});

template<typename T>
void swap(T& a, T& b) { auto tmp = a; a = b; b = tmp; }

namespace parallel {
namespace gl {

struct { compute_program histogram_count, prefix_scan, permute, flip_float; } static kernels;
struct { GLuint consts, histogram, output[2]; } static buffers;

void radix_sort(GL const & gl, GLuint key, GLsizeiptr size, GLuint index /*= 0*/,
  bool descending /*=  false*/, bool is_signed /*=  false*/, bool is_float /*=  false*/) {
  struct Consts { GLuint shift, descending, is_signed, key_index; } consts = { 0, descending, 0, index != 0 };
  static bool initialized = false;
  if (!initialized) {
    gl.GenBuffers(sizeof(buffers) / sizeof(GLuint), reinterpret_cast<GLuint *>(&buffers));
    gl.BindBuffer(GL_COPY_WRITE_BUFFER, buffers.consts);
    gl.BufferData(GL_COPY_WRITE_BUFFER, sizeof(consts), nullptr, GL_DYNAMIC_DRAW);
    gl.BindBuffer(GL_COPY_WRITE_BUFFER, buffers.histogram);
    gl.BufferData(GL_COPY_WRITE_BUFFER, sizeof(GLuint) * WG_COUNT * RADICES, nullptr, GL_DYNAMIC_COPY);

    kernels.histogram_count = make_program<GL_COMPUTE_SHADER>(gl, prolog, histogram_count);
    kernels.prefix_scan = make_program<GL_COMPUTE_SHADER>(gl, prolog, prefix_scan);
    kernels.permute = make_program<GL_COMPUTE_SHADER>(gl, prolog, permute);
    kernels.flip_float = make_program<GL_COMPUTE_SHADER>(gl, prolog, flip_float);
    initialized = true;
  }

  gl.BindBuffer(GL_COPY_WRITE_BUFFER, buffers.output[0]);
  gl.BufferData(GL_COPY_WRITE_BUFFER, sizeof(GLuint) * size, nullptr, GL_DYNAMIC_COPY);
  if (index != 0) {
    gl.BindBuffer(GL_COPY_WRITE_BUFFER, buffers.output[1]);
    gl.BufferData(GL_COPY_WRITE_BUFFER, sizeof(GLuint) * size, nullptr, GL_DYNAMIC_COPY);
  }

  GLuint data[] = { key, buffers.output[0], index, index != 0 ? buffers.output[1] : 0 };
  gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, HISTOGRAM, buffers.histogram);
  for (GLuint ib = 0; ib < 32; ib+=4) {
    consts.shift = ib;
    consts.is_signed = is_signed && !is_float && ib == 28;
    gl.BindBuffer(GL_COPY_WRITE_BUFFER, buffers.consts);
    *static_cast<Consts *>(gl.MapBuffer(GL_COPY_WRITE_BUFFER, GL_WRITE_ONLY)) = consts;
    gl.UnmapBuffer(GL_COPY_WRITE_BUFFER);
    gl.BindBufferBase(GL_UNIFORM_BUFFER, CONSTS, buffers.consts);
    gl.BindBufferRange(GL_SHADER_STORAGE_BUFFER, DATA + KEY_IN, data[KEY_IN], 0, sizeof(GLuint) * size);
    gl.BindBufferRange(GL_SHADER_STORAGE_BUFFER, DATA + KEY_OUT, data[KEY_OUT], 0, sizeof(GLuint) * size);
    gl.BindBufferRange(GL_SHADER_STORAGE_BUFFER, DATA + VALUE_IN, data[VALUE_IN], 0, sizeof(GLuint) * size);
    gl.BindBufferRange(GL_SHADER_STORAGE_BUFFER, DATA + VALUE_OUT, data[VALUE_OUT], 0, sizeof(GLuint) * size);

    if (is_float && ib == 0)
      kernels.flip_float.dispatch(gl, WG_COUNT);

    gl.MemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    kernels.histogram_count.dispatch(gl, WG_COUNT);
    gl.MemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    kernels.prefix_scan.dispatch(gl);
    gl.MemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    kernels.permute.dispatch(gl, WG_COUNT);
    gl.MemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    swap(data[KEY_IN], data[KEY_OUT]);
    swap(data[VALUE_IN], data[VALUE_OUT]);
  }

  if (is_float) {
    consts.is_signed = true;
    gl.BindBuffer(GL_COPY_WRITE_BUFFER, buffers.consts);
    *static_cast<Consts *>(gl.MapBuffer(GL_COPY_WRITE_BUFFER, GL_WRITE_ONLY)) = consts;
    gl.UnmapBuffer(GL_COPY_WRITE_BUFFER);
    gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, DATA + KEY_IN, data[KEY_IN]);
    kernels.flip_float.dispatch(gl, WG_COUNT);
  }

  gl.BindBuffer(GL_COPY_WRITE_BUFFER, buffers.output[0]);
  gl.BufferData(GL_COPY_WRITE_BUFFER, 0, nullptr, GL_DYNAMIC_COPY);
  if (index != 0) {
    gl.BindBuffer(GL_COPY_WRITE_BUFFER, buffers.output[1]);
    gl.BufferData(GL_COPY_WRITE_BUFFER, 0, nullptr, GL_DYNAMIC_COPY);
  }
}

}
}