"""Run real adaptor + installed VSAG, replacing only OB logging/clock plumbing.
This is an isolated component check, not a seekdb server integration test.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
workspace = tempfile.TemporaryDirectory(prefix='seekdb-hipvs-component-')
out = Path(workspace.name)
stubs = out / 'stubs'
common = r'''
#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#define OB_FAIL(x) ((ret = (x)) != 0)
#define OB_SUCC(x) ((x) == 0)
#define OB_ISNULL(x) ((x) == nullptr)
#define OB_UNLIKELY(x) (x)
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#define LOG_TRACE(...) ((void)0)
#define LOG_USER_ERROR(...) ((void)0)
#define TO_STRING_KV(...)
#define MAX(a,b) ((a) > (b) ? (a) : (b))
namespace oceanbase { namespace common {
enum { OB_SUCCESS = 0, OB_ERR_UNEXPECTED = -4000, OB_ERR_VSAG_RETURN_ERROR = -4001,
OB_INVALID_ARGUMENT = -4002, OB_NOT_SUPPORTED = -4003, OB_OP_NOT_ALLOW = -4004,
OB_ALLOCATE_MEMORY_FAILED = -4005 };
inline int databuff_printf(char* buffer, int64_t size, int64_t& pos, const char* fmt, ...) {
  va_list args; va_start(args, fmt); int n = vsnprintf(buffer+pos, size-pos, fmt, args); va_end(args);
  if (n < 0 || n >= size-pos) return -1; pos += n; return 0;
}
}}
'''
for path in ['lib/utility/ob_print_utils.h', 'lib/oblog/ob_log.h', 'lib/worker.h']:
    dest = stubs / path
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(common if path.endswith('ob_print_utils.h') else '#pragma once\n')
clock = stubs / 'lib/time/ob_time_utility.h'
clock.parent.mkdir(parents=True, exist_ok=True)
clock.write_text('''#pragma once
#include <chrono>
namespace oceanbase { namespace common {
struct ObTimeUtility { static int64_t current_time() {
return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); } };
}}
''')
main = r'''
#include "lib/vector/ob_vsag_adaptor.h"
#include "vsag/vsag.h"
#include <cassert>
#include <sstream>
#include <vector>
#include <cstdio>
#include <cstring>
#ifdef OB_BUILD_CUVS
static bool raw_snapshot_mode = false;
static int build_calls = 0, serialize_calls = 0, deserialize_calls = 0;
extern "C" {
void* seekdb_cuvs_build(const float*, long, long) {
  ++build_calls; return raw_snapshot_mode ? reinterpret_cast<void*>(1) : nullptr;
}
int seekdb_cuvs_search(void*, const float*, long, long, unsigned*, float*) { return 1; }
void seekdb_cuvs_free(void*) {}
int seekdb_cuvs_cagra_knn(const float*,long,long,const float*,long,long,unsigned*) { return 1; }
int seekdb_cuvs_serialize(void*,const char*) { ++serialize_calls; return 1; }
void* seekdb_cuvs_deserialize(const char*,long,long) { ++deserialize_calls; return nullptr; }
int seekdb_cuvs_raw_snapshot(void* handle) { return raw_snapshot_mode && handle != nullptr ? 1 : 0; }
}
#endif
struct Allocator : vsag::Allocator {
 std::string Name() override { return "component-test"; }
 void* Allocate(uint64_t n) override { return malloc(n); }
 void Deallocate(void* p) override { free(p); }
 void* Reallocate(void* p,uint64_t n) override { return realloc(p,n); }
};
int main() {
 using namespace oceanbase::common;
 Allocator allocator;
 constexpr int32_t dim=4, reserved=0;
 constexpr int64_t n=256;
 uint64_t bytes=16;
 std::vector<int64_t> ids(n); std::vector<float> data(n*dim);
 for (int row=0;row<n;++row) { ids[row]=1000+row*7; for(int col=0;col<dim;++col) data[row*dim+col]=row+col*0.01f; }
 std::stringstream snapshot(std::ios::in|std::ios::out|std::ios::binary);
 snapshot.write("OBCUVS02",8);
 snapshot.write((const char*)&bytes,8); snapshot.write((const char*)&dim,4);
 snapshot.write((const char*)&reserved,4); snapshot.write((const char*)&n,8);
 snapshot.write((const char*)ids.data(),n*8); snapshot.write((const char*)data.data(),n*dim*4);
 snapshot.write("opaque-graph-1234",16);
 obvsag::VectorIndexPtr handler=nullptr;
 assert(obvsag::create_index(handler,obvsag::HNSW_TYPE,"float32","l2",dim,16,100,100,&allocator)==0);
 assert(obvsag::fdeserialize(handler,snapshot)==0);
 int64_t count=0; assert(obvsag::get_index_number(handler,count)==0 && count==n);
 const float* dist=nullptr;const int64_t* found_ids=nullptr;const char* extra=nullptr;int64_t found=0;
 assert(obvsag::knn_search(handler,data.data(),dim,10,dist,found_ids,found,100,false,extra)==0);
 assert(found==10 && found_ids[0]==ids[0] && dist[0]<0.0001f);
 allocator.Deallocate((void*)dist);allocator.Deallocate((void*)found_ids);
 std::stringstream cpu; assert(obvsag::fserialize(handler,cpu)==0);
 assert(cpu.str().substr(0,8)!="OBCUVS02");
 int64_t new_id=999999;float added[dim]={-1,-1,-1,-1};
 // Reproduce the plugin's per-insert marking after CPU recovery.
 obvsag::mark_cuvs_index(handler);
 assert(obvsag::add_index(handler,added,&new_id,dim,1)==0);
 assert(obvsag::get_index_number(handler,count)==0 && count==n+1);
 assert(obvsag::delete_index(handler)==0);
 std::string truncated=snapshot.str();truncated.pop_back();std::stringstream bad(truncated);
 assert(obvsag::create_index(handler,obvsag::HNSW_TYPE,"float32","l2",dim,16,100,100,&allocator)==0);
 assert(obvsag::fdeserialize(handler,bad)!=0);assert(obvsag::delete_index(handler)==0);
#ifdef OB_BUILD_CUVS
 assert(obvsag::create_index(handler,obvsag::HNSW_TYPE,"float32","l2",dim,16,100,100,&allocator)==0);
 obvsag::mark_cuvs_index(handler);
 assert(obvsag::add_index(handler,data.data(),ids.data(),dim,n)==0);
 std::stringstream fallback;assert(obvsag::fserialize(handler,fallback)==0);
 assert(fallback.str().substr(0,8)!="OBCUVS02");
 assert(obvsag::delete_index(handler)==0);
 assert(obvsag::create_index(handler,obvsag::HNSW_TYPE,"float32","l2",dim,16,100,100,&allocator)==0);
 obvsag::mark_cuvs_index(handler);
 assert(obvsag::fdeserialize(handler,fallback)==0);
 obvsag::mark_cuvs_index(handler);
 assert(obvsag::knn_search(handler,data.data(),dim,10,dist,found_ids,found,100,false,extra)==0);
 assert(found==10 && found_ids[0]==ids[0]);
 allocator.Deallocate((void*)dist);allocator.Deallocate((void*)found_ids);
 assert(obvsag::delete_index(handler)==0);
 raw_snapshot_mode=true;
 assert(obvsag::create_index(handler,obvsag::HNSW_TYPE,"float32","l2",dim,16,100,100,&allocator)==0);
 obvsag::mark_cuvs_index(handler);
 assert(obvsag::add_index(handler,data.data(),ids.data(),dim,n)==0);
 std::stringstream raw_stream(std::ios::in|std::ios::out|std::ios::binary);
 assert(obvsag::fserialize(handler,raw_stream)==0);
 const std::string raw=raw_stream.str();
 uint64_t raw_graph_bytes=UINT64_MAX;
 std::memcpy(&raw_graph_bytes,raw.data()+8,sizeof(raw_graph_bytes));
 assert(raw.substr(0,8)=="OBCUVS02" && raw_graph_bytes==0);
 assert(raw.size()==32+n*sizeof(int64_t)+n*dim*sizeof(float));
 assert(serialize_calls==0);
 assert(obvsag::delete_index(handler)==0);
 const int builds_before_raw_load=build_calls, loads_before_raw_load=deserialize_calls;
 assert(obvsag::create_index(handler,obvsag::HNSW_TYPE,"float32","l2",dim,16,100,100,&allocator)==0);
 raw_stream.seekg(0);assert(obvsag::fdeserialize(handler,raw_stream)==0);
 assert(build_calls==builds_before_raw_load+1 && deserialize_calls==loads_before_raw_load);
 assert(obvsag::get_index_number(handler,count)==0 && count==n);
 assert(obvsag::delete_index(handler)==0);
 std::string truncated_raw=raw;truncated_raw.pop_back();std::stringstream bad_raw(truncated_raw);
 assert(obvsag::create_index(handler,obvsag::HNSW_TYPE,"float32","l2",dim,16,100,100,&allocator)==0);
 assert(obvsag::fdeserialize(handler,bad_raw)!=0);assert(obvsag::delete_index(handler)==0);
 raw_snapshot_mode=false;
 std::stringstream cpu_raw(raw);
 assert(obvsag::create_index(handler,obvsag::HNSW_TYPE,"float32","l2",dim,16,100,100,&allocator)==0);
 assert(obvsag::fdeserialize(handler,cpu_raw)==0);
 assert(obvsag::get_index_number(handler,count)==0 && count==n);
 assert(obvsag::delete_index(handler)==0);
#endif
 puts("PASS actual adaptor + VSAG snapshot recovery, query, insertion, CPU serialization and truncation checks");
}
'''
(out / 'main.cpp').write_text(main)
deps = Path(os.environ.get('SEEKDB_DEPS', str(root / 'deps/3rd/usr/local/oceanbase/deps/devel')))
libdir = deps / 'lib/vsag_lib'
runtime = out / 'runtime'
runtime.mkdir(exist_ok=True)
aio = runtime / 'libaio.so.1'
if not aio.exists() and Path('/usr/lib/x86_64-linux-gnu/libaio.so.1t64').exists():
    aio.symlink_to('/usr/lib/x86_64-linux-gnu/libaio.so.1t64')
env = dict(os.environ, LD_LIBRARY_PATH=f'{runtime}:{libdir}', UBSAN_OPTIONS='halt_on_error=1')
for enabled in (False, True):
    binary = out / ('adaptor_gpu_unavailable' if enabled else 'adaptor_cpu')
    command = [os.environ.get('CXX', 'g++'), '-std=c++20', '-g', '-O1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               '-I'+str(stubs), '-I'+str(root / 'src/oblib'), '-I'+str(deps / 'include'),
               str(root / 'src/oblib/lib/vector/ob_vsag_adaptor.cpp'), str(out/'main.cpp'),
               '-L'+str(libdir), '-lvsag', '-lpthread', '-fopenmp', '-o', str(binary)]
    if enabled:
        command.append('-DOB_BUILD_CUVS')
    subprocess.run(command, check=True, env=env)
    print('running component variant GPU_ENABLED=', enabled, flush=True)
    subprocess.run([str(binary)], check=True, env=env)
