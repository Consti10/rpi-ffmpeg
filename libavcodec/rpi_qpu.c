// This works better than the mmap in that the memory can be cached, but requires a kernel modification to enable the device.
// define RPI_TIME_TOTAL_QPU to print out how much time is spent in the QPU code
#define RPI_TIME_TOTAL_QPU
// define RPI_TIME_TOTAL_VPU to print out how much time is spent in the VPI code
//#define RPI_TIME_TOTAL_VPU
// define RPI_ASYNC to run the VPU in a separate thread, need to make a separate call to check for completion
#define RPI_ASYNC

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <assert.h>

#include <pthread.h>
#include <time.h>

#include "rpi_mailbox.h"
#include "rpi_qpu.h"
#include "rpi_shader.h"
#include "rpi_hevc_transform.h"

#include "rpi_user_vcsm.h"

// On Pi2 there is no way to access the VPU L2 cache
// GPU_MEM_FLG should be 4 for uncached memory.  (Or C for alias to allocate in the VPU L2 cache)
// However, if using VCSM allocated buffers, need to use C at the moment because VCSM does not allocate uncached memory correctly
// The QPU crashes if we mix L2 cached and L2 uncached accesses due to a HW bug. 
#define GPU_MEM_FLG 0x4
// GPU_MEM_MAP is meaningless on the Pi2 and should be left at 0  (On Pi1 it allows ARM to access VPU L2 cache)
#define GPU_MEM_MAP 0x0

#define vcos_verify(x) ((x)>=0)

typedef unsigned char uint8_t;
typedef signed char int8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef int int32_t;

/*static const unsigned code[] =
{
  #include "rpi_shader.hex"
};*/

// Size in 32bit words
#define QPU_CODE_SIZE 2048
#define VPU_CODE_SIZE 2048

const short rpi_transMatrix2even[32][16] = { // Even rows first
{64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64},
{90,  87,  80,  70,  57,  43,  25,   9,  -9, -25, -43, -57, -70, -80, -87, -90},
{89,  75,  50,  18, -18, -50, -75, -89, -89, -75, -50, -18,  18,  50,  75,  89},
{87,  57,   9, -43, -80, -90, -70, -25,  25,  70,  90,  80,  43,  -9, -57, -87},
{83,  36, -36, -83, -83, -36,  36,  83,  83,  36, -36, -83, -83, -36,  36,  83},
{80,   9, -70, -87, -25,  57,  90,  43, -43, -90, -57,  25,  87,  70,  -9, -80},
{75, -18, -89, -50,  50,  89,  18, -75, -75,  18,  89,  50, -50, -89, -18,  75},
{70, -43, -87,   9,  90,  25, -80, -57,  57,  80, -25, -90,  -9,  87,  43, -70},
{64, -64, -64,  64,  64, -64, -64,  64,  64, -64, -64,  64,  64, -64, -64,  64},
{57, -80, -25,  90,  -9, -87,  43,  70, -70, -43,  87,   9, -90,  25,  80, -57},
{50, -89,  18,  75, -75, -18,  89, -50, -50,  89, -18, -75,  75,  18, -89,  50},
{43, -90,  57,  25, -87,  70,   9, -80,  80,  -9, -70,  87, -25, -57,  90, -43},
{36, -83,  83, -36, -36,  83, -83,  36,  36, -83,  83, -36, -36,  83, -83,  36},
{25, -70,  90, -80,  43,   9, -57,  87, -87,  57,  -9, -43,  80, -90,  70, -25},
{18, -50,  75, -89,  89, -75,  50, -18, -18,  50, -75,  89, -89,  75, -50,  18},
{ 9, -25,  43, -57,  70, -80,  87, -90,  90, -87,  80, -70,  57, -43,  25,  -9},
// Odd rows
{90,  90,  88,  85,  82,  78,  73,  67,  61,  54,  46,  38,  31,  22,  13,   4},
{90,  82,  67,  46,  22,  -4, -31, -54, -73, -85, -90, -88, -78, -61, -38, -13},
{88,  67,  31, -13, -54, -82, -90, -78, -46,  -4,  38,  73,  90,  85,  61,  22},
{85,  46, -13, -67, -90, -73, -22,  38,  82,  88,  54,  -4, -61, -90, -78, -31},
{82,  22, -54, -90, -61,  13,  78,  85,  31, -46, -90, -67,   4,  73,  88,  38},
{78,  -4, -82, -73,  13,  85,  67, -22, -88, -61,  31,  90,  54, -38, -90, -46},
{73, -31, -90, -22,  78,  67, -38, -90, -13,  82,  61, -46, -88,  -4,  85,  54},
{67, -54, -78,  38,  85, -22, -90,   4,  90,  13, -88, -31,  82,  46, -73, -61},
{61, -73, -46,  82,  31, -88, -13,  90,  -4, -90,  22,  85, -38, -78,  54,  67},
{54, -85,  -4,  88, -46, -61,  82,  13, -90,  38,  67, -78, -22,  90, -31, -73},
{46, -90,  38,  54, -90,  31,  61, -88,  22,  67, -85,  13,  73, -82,   4,  78},
{38, -88,  73,  -4, -67,  90, -46, -31,  85, -78,  13,  61, -90,  54,  22, -82},
{31, -78,  90, -61,   4,  54, -88,  82, -38, -22,  73, -90,  67, -13, -46,  85},
{22, -61,  85, -90,  73, -38,  -4,  46, -78,  90, -82,  54, -13, -31,  67, -88},
{13, -38,  61, -78,  88, -90,  85, -73,  54, -31,   4,  22, -46,  67, -82,  90},
{ 4, -13,  22, -31,  38, -46,  54, -61,  67, -73,  78, -82,  85, -88,  90, -90}
};

struct GPU
{
  unsigned int qpu_code[QPU_CODE_SIZE];
  unsigned int vpu_code[VPU_CODE_SIZE];
  short transMatrix2even[16*16*2];
  int open_count; // Number of allocated video buffers
  int      mb; // Mailbox handle
  int      vc; // Address in GPU memory
  int mail[12]; // These are used to pass pairs of code/unifs to the QPUs
};

// Stop more than one thread trying to allocate memory or use the processing resources at once
static pthread_mutex_t gpu_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile struct GPU* gpu = NULL;
static GPU_MEM_PTR_T gpu_mem_ptr;

#if defined(RPI_TIME_TOTAL_QPU) || defined(RPI_TIME_TOTAL_VPU)
static unsigned int Microseconds(void) {
    struct timespec ts;
    unsigned int x;
    static unsigned int base = 0;
    clock_gettime(CLOCK_REALTIME, &ts);
    x = ts.tv_sec*1000000 + ts.tv_nsec/1000;
    if (base==0) base=x;
    return x-base;
}
#endif

#ifdef RPI_ASYNC
pthread_t vpu_thread;
static void *vpu_start(void *arg);

#define MAXCMDS 128
static pthread_cond_t post_cond_head = PTHREAD_COND_INITIALIZER;
static pthread_cond_t post_cond_tail = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t post_mutex = PTHREAD_MUTEX_INITIALIZER;

static int vpu_cmds[MAXCMDS][16];
static volatile int vpu_async_tail=0; // Contains the number of posted jobs
static volatile int vpu_async_head=0;
#endif

static int gpu_malloc_uncached_internal(int numbytes, GPU_MEM_PTR_T *p, int mb);
static void gpu_free_internal(GPU_MEM_PTR_T *p);

// Connect to QPU, returns 0 on success.
static int gpu_init(volatile struct GPU **gpu) {
  int mb = mbox_open();
  int vc;
  volatile struct GPU* ptr;
	if (mb < 0)
		return -1;

	if (qpu_enable(mb, 1)) return -2;
  
  vcsm_init();
  gpu_malloc_uncached_internal(sizeof(struct GPU), &gpu_mem_ptr, mb);
  ptr = (volatile struct GPU*)gpu_mem_ptr.arm;
  memset(ptr, 0, sizeof *ptr);
  vc = gpu_mem_ptr.vc;

  ptr->mb = mb;
  ptr->vc = vc;
  
  printf("GPU allocated at 0x%x\n",vc);
  
  *gpu = ptr;
  
  // Now copy over the QPU code into GPU memory
  {
    int num_bytes = qpu_get_fn(QPU_MC_END) - qpu_get_fn(QPU_MC_SETUP_UV);
    assert(num_bytes<=QPU_CODE_SIZE*sizeof(unsigned int));
    memcpy((void*)ptr->qpu_code, rpi_shader, num_bytes);
  }
  // And the VPU code
  {
    int num_bytes = sizeof(rpi_hevc_transform);
    assert(num_bytes<=VPU_CODE_SIZE*sizeof(unsigned int));
    memcpy((void*)ptr->vpu_code, rpi_hevc_transform, num_bytes);
  }
  // And the transform coefficients
  memcpy((void*)ptr->transMatrix2even, rpi_transMatrix2even, sizeof(rpi_transMatrix2even));
  
#ifdef RPI_ASYNC
  {
    int err;
    vpu_async_tail = 0;
    vpu_async_head = 0;
    err = pthread_create(&vpu_thread, NULL, vpu_start, NULL);
    //printf("Created thread\n");
    if (err) {
        printf("Failed to create vpu thread\n");
        return -4;
    }
  }
#endif
  
  return 0;
}

// Returns 1 if the gpu is currently idle
static int gpu_idle(void)
{
  int ret = pthread_mutex_trylock(&gpu_mutex);
  if (ret==0) {
    pthread_mutex_unlock(&gpu_mutex);
    return 1;
  }
  return 0;
}

// Make sure we have exclusive access to the mailbox, and enable qpu if necessary.
static void gpu_lock(void) {
  pthread_mutex_lock(&gpu_mutex);
  
  if (gpu==NULL) {
    gpu_init(&gpu);
  }
}

static void gpu_unlock(void) {
  pthread_mutex_unlock(&gpu_mutex);
}

static int gpu_malloc_uncached_internal(int numbytes, GPU_MEM_PTR_T *p, int mb) {
  p->vcsm_handle = vcsm_malloc_cache(numbytes, VCSM_CACHE_TYPE_NONE, (char *)"Video Frame" );
  assert(p->vcsm_handle);
  p->vc_handle = vcsm_vc_hdl_from_hdl(p->vcsm_handle);
  assert(p->vc_handle);
  p->arm = vcsm_lock(p->vcsm_handle);  
  assert(p->arm);
  p->vc = mem_lock(mb, p->vc_handle);
  assert(p->vc);
  return 0;
}

// Allocate memory on GPU
// Fills in structure <p> containing ARM pointer, videocore handle, videocore memory address, numbytes
// Returns 0 on success.
// This allocates memory that will not be cached in ARM's data cache.
// Therefore safe to use without data cache flushing.
int gpu_malloc_uncached(int numbytes, GPU_MEM_PTR_T *p)
{
  int r;
  gpu_lock();
  r = gpu_malloc_uncached_internal(numbytes, p, gpu->mb);
  gpu->open_count++;
  gpu_unlock();
  return r;
}

int gpu_get_mailbox(void)
{
  assert(gpu);
  return gpu->mb;
}

void gpu_cache_flush(GPU_MEM_PTR_T *p)
{
  void *tmp = vcsm_lock(p->vcsm_handle); 
  vcsm_unlock_ptr(tmp);
}

static int gpu_malloc_cached_internal(int numbytes, GPU_MEM_PTR_T *p) {
  p->vcsm_handle = vcsm_malloc_cache(numbytes, VCSM_CACHE_TYPE_HOST, (char *)"Video Frame" );
  //p->vcsm_handle = vcsm_malloc_cache(numbytes, VCSM_CACHE_TYPE_VC, (char *)"Video Frame" ); 
  //p->vcsm_handle = vcsm_malloc_cache(numbytes, VCSM_CACHE_TYPE_NONE, (char *)"Video Frame" );
  //p->vcsm_handle = vcsm_malloc_cache(numbytes, VCSM_CACHE_TYPE_HOST_AND_VC, (char *)"Video Frame" );
  assert(p->vcsm_handle);
  p->vc_handle = vcsm_vc_hdl_from_hdl(p->vcsm_handle);
  assert(p->vc_handle);
  p->arm = vcsm_lock(p->vcsm_handle);  
  assert(p->arm);
  p->vc = mem_lock(gpu->mb, p->vc_handle);
  assert(p->vc);
  return 0;
}

// This allocates data that will be
//    Cached in ARM L2
//    Uncached in VPU L2
int gpu_malloc_cached(int numbytes, GPU_MEM_PTR_T *p)
{
  int r;
  gpu_lock();
  r = gpu_malloc_cached_internal(numbytes, p);
  gpu->open_count++;
  gpu_unlock();
  return r;
}

static void gpu_term(void)
{
  int mb;
  
  if (gpu==NULL) 
    return;
  mb = gpu->mb;
  
#ifdef RPI_ASYNC
  {
    void *res;
    vpu_post_code(0, 0, 0, 0, 0, 0, -1, NULL);
    pthread_join(vpu_thread, &res);    
  }
#endif
    
  qpu_enable(mb, 0);
  gpu_free_internal(&gpu_mem_ptr);

  vcsm_exit();

  mbox_close(mb);
  gpu = NULL;
}

void gpu_free_internal(GPU_MEM_PTR_T *p) {
  int mb = gpu->mb;
  mem_unlock(mb,p->vc_handle);
  vcsm_unlock_ptr(p->arm);
  vcsm_free(p->vcsm_handle);
}

void gpu_free(GPU_MEM_PTR_T *p) {
  gpu_lock();

  gpu_free_internal(p);

  gpu->open_count--;
  if (gpu->open_count==0) {
      printf("Closing GPU\n");
      gpu_term();
      gpu = NULL;
  }
  gpu_unlock();
}

unsigned int vpu_get_fn(void) {
  // Make sure that the gpu is initialized
  if (gpu==NULL) {
    printf("Preparing gpu\n");
    gpu_lock();
    gpu_unlock();
  }
  return gpu->vc + offsetof(struct GPU,vpu_code);
}

unsigned int vpu_get_constants(void) {
  if (gpu==NULL) {
    gpu_lock();
    gpu_unlock();
  }
  return gpu->vc + offsetof(struct GPU,transMatrix2even);
}

#ifdef RPI_ASYNC

static void *vpu_start(void *arg) {
  while(1) {
    int *p;
    int qpu_code;
    pthread_mutex_lock(&post_mutex);
    while( vpu_async_tail - vpu_async_head <= 0)
    {
      //printf("Checking number %d %d\n",vpu_async_head,vpu_async_tail);
      pthread_cond_wait(&post_cond_tail, &post_mutex);
    }
    p = vpu_cmds[vpu_async_head%MAXCMDS];
    pthread_mutex_unlock(&post_mutex);
    
    if (p[6] == -1) {
      break; // Last job
    }
    qpu_code = p[7];
    //if (p[7]) {
        //GPU_MEM_PTR_T *buf = (GPU_MEM_PTR_T *)p[7];
        //gpu_cache_flush(buf);
    //}
    if (!qpu_code) {
      vpu_execute_code(p[0], p[1], p[2], p[3], p[4], p[5], p[6]);
    } else {
      int i;
      for(i=0;i<8;i++) {
        gpu->mail[i*2] = p[8+i];
        gpu->mail[i*2 + 1] = qpu_code;  
      }
     
      execute_multi(gpu->mb,8,gpu->vc + offsetof(struct GPU, mail), 1 /* no flush */, 5000 /* timeout ms */,
                              0, 0, 0, 0,
                              p[0], p[1], p[2], p[3], p[4], p[5], p[6], // VPU0
                              0,    0   , 0   , 0   , 0   , 0   , 0); // VPU1
    }
    pthread_mutex_lock(&post_mutex);
    vpu_async_head++;
    pthread_cond_broadcast(&post_cond_head);
    pthread_mutex_unlock(&post_mutex);
  }
  
  return NULL;
}

// Post a command to the queue
// Returns an id which we can use to wait for completion
int vpu_post_code(unsigned code, unsigned r0, unsigned r1, unsigned r2, unsigned r3, unsigned r4, unsigned r5, GPU_MEM_PTR_T *buf)
{
  // If the gpu is idle then just run the command immediately
  // This works, but doesn't seem to give any benefit
  // if (gpu_idle()) {
  //   vpu_execute_code( code,  r0,  r1,  r2,  r3,  r4,  r5);
  //   return -1; // TODO perhaps a wraparound bug here?
  // }
    
  pthread_mutex_lock(&post_mutex);
  {
    int id = vpu_async_tail++;
    int *p = vpu_cmds[id%MAXCMDS];
    int num = vpu_async_tail - vpu_async_head;
    if (num>MAXCMDS) {
      printf("Too many commands submitted\n");
      exit(-1);
    }
    p[0] = code;
    p[1] = r0;
    p[2] = r1;
    p[3] = r2;
    p[4] = r3;
    p[5] = r4;
    p[6] = r5;
    p[7] = 0;
    if (num<=1)
      pthread_cond_broadcast(&post_cond_tail); // Otherwise the vpu thread must already be awake
    pthread_mutex_unlock(&post_mutex);
    return id;
  }
}

int vpu_qpu_post_code(unsigned vpu_code, unsigned r0, unsigned r1, unsigned r2, unsigned r3, unsigned r4, unsigned r5, 
                      int qpu_code, int unifs1, int unifs2, int unifs3, int unifs4, int unifs5, int unifs6, int unifs7, int unifs8)
{
    
  pthread_mutex_lock(&post_mutex);
  {
    int id = vpu_async_tail++;
    int *p = vpu_cmds[id%MAXCMDS];
    int num = vpu_async_tail - vpu_async_head;
    if (num>MAXCMDS) {
      printf("Too many commands submitted\n");
      exit(-1);
    }
    p[0] = vpu_code;
    p[1] = r0;
    p[2] = r1;
    p[3] = r2;
    p[4] = r3;
    p[5] = r4;
    p[6] = r5;
    p[7] = qpu_code;
    p[8 ] = unifs1;
    p[9 ] = unifs2;
    p[10] = unifs3;
    p[11] = unifs4;
    p[12] = unifs5;
    p[13] = unifs6;
    p[14] = unifs7;
    p[15] = unifs8;
    if (num<=1)
      pthread_cond_broadcast(&post_cond_tail); // Otherwise the vpu thread must already be awake
    pthread_mutex_unlock(&post_mutex);
    return id;
  }
}

// Wait for completion of the given command
void vpu_wait(int id)
{
  pthread_mutex_lock(&post_mutex);
  while( id + 1 - vpu_async_head > 0)
  {
    pthread_cond_wait(&post_cond_head, &post_mutex);
  }
  pthread_mutex_unlock(&post_mutex);
}

#endif


unsigned vpu_execute_code( unsigned code, unsigned r0, unsigned r1, unsigned r2, unsigned r3, unsigned r4, unsigned r5)
{
  unsigned r;
#ifdef RPI_TIME_TOTAL_VPU
  static int last_time=0;
  static long long on_time=0;
  static long long off_time=0;
  int start_time;
  int end_time;
  static int count=0;
  static long long countr2=0;
#endif
#ifndef RPI_ASYNC
  gpu_lock();
#endif
#ifdef RPI_TIME_TOTAL_VPU
  start_time = Microseconds();
  if (last_time==0)
    last_time = start_time;
  off_time += start_time-last_time;
#endif
  r = execute_code(gpu->mb, code, r0, r1, r2, r3, r4, r5);
#ifdef RPI_TIME_TOTAL_VPU
  end_time = Microseconds();
  last_time = end_time;
  on_time += end_time - start_time;
  count++;
  countr2 += r2;
  if ((count&0x7f)==0)
    printf("VPU %d %lld On=%dms, Off=%dms\n",count,countr2,(int)(on_time/1000),(int)(off_time/1000));
#endif
#ifndef RPI_ASYNC
  gpu_unlock();
#endif
  return r;
}

// Run a program on a QPU with the given code and uniform stream (given in GPU addresses)
// The first num QPUs will start at code, the next num2 QPUs will start at code2
void qpu_run_shader12(int code, int num, int code2, int num2, int unifs1, int unifs2, int unifs3, int unifs4, int unifs5, int unifs6, int unifs7, int unifs8, int unifs9, int unifs10, int unifs11, int unifs12)
{
  int i;
#ifdef RPI_TIME_TOTAL_QPU
  static int last_time=0;
  static long long on_time=0;
  static long long off_time=0;
  int start_time;
  int end_time;
  static int count=0;
#endif

  gpu_lock();
#ifdef RPI_TIME_TOTAL_QPU
  start_time = Microseconds();
  if (last_time==0)
    last_time = start_time;
  off_time += start_time-last_time;
#endif
  for(i=0;i<num;i++) {
    gpu->mail[i*2 + 1] = code;  
  }
  for(;i<num+num2;i++) {
    gpu->mail[i*2 + 1] = code2;  
  }
  gpu->mail[0 ] = unifs1;
  gpu->mail[2 ] = unifs2;
  gpu->mail[4 ] = unifs3;
  gpu->mail[6 ] = unifs4;
  gpu->mail[8 ] = unifs5;
  gpu->mail[10] = unifs6;
	gpu->mail[12] = unifs7;
	gpu->mail[14] = unifs8;
	gpu->mail[16] = unifs9;
	gpu->mail[18] = unifs10;
	gpu->mail[20] = unifs11;
	gpu->mail[22] = unifs12;
	execute_qpu(
		gpu->mb,
		12 /* Number of QPUs */,
		gpu->vc + offsetof(struct GPU, mail),
		1 /* no flush */,  // Don't flush VPU L1 cache
		5000 /* timeout ms */);
#ifdef RPI_TIME_TOTAL_QPU
  end_time = Microseconds();
  last_time = end_time;
  on_time += end_time - start_time;
  count++;
  if ((count&0x7f)==0)
    printf("On=%dms, Off=%dms\n",(int)(on_time/1000),(int)(off_time/1000));
#endif
  gpu_unlock();
}

// Run a program on 8 QPUs with the given code and uniform stream (given in GPU addresses)
void qpu_run_shader8(int code, int unifs1, int unifs2, int unifs3, int unifs4, int unifs5, int unifs6, int unifs7, int unifs8)
{
  int i;
#ifdef RPI_TIME_TOTAL_QPU
  static int last_time=0;
  static long long on_time=0;
  static long long off_time=0;
  int start_time;
  int end_time;
  static int count=0;
#endif

  gpu_lock();
#ifdef RPI_TIME_TOTAL_QPU
  start_time = Microseconds();
  if (last_time==0)
    last_time = start_time;
  off_time += start_time-last_time;
#endif
  for(i=0;i<8;i++) {
    gpu->mail[i*2 + 1] = code;  
  }
  gpu->mail[0 ] = unifs1;
  gpu->mail[2 ] = unifs2;
  gpu->mail[4 ] = unifs3;
  gpu->mail[6 ] = unifs4;
  gpu->mail[8 ] = unifs5;
  gpu->mail[10] = unifs6;
	gpu->mail[12] = unifs7;
	gpu->mail[14] = unifs8;
	execute_qpu(
		gpu->mb,
		8 /* Number of QPUs */,
		gpu->vc + offsetof(struct GPU, mail),
		1 /* no flush */,  // Don't flush VPU L1 cache
		5000 /* timeout ms */);
#ifdef RPI_TIME_TOTAL_QPU
  end_time = Microseconds();
  last_time = end_time;
  on_time += end_time - start_time;
  count++;
  if ((count&0x7f)==0)
    printf("On=%dms, Off=%dms\n",(int)(on_time/1000),(int)(off_time/1000));
#endif
  gpu_unlock();
}
  
unsigned int qpu_get_fn(int num) {
    // Make sure that the gpu is initialized
    unsigned int *fn;
    if (gpu==NULL) {
      printf("Preparing gpu\n");
      gpu_lock();
      gpu_unlock();
    }
    switch(num) {
    //case QPU_MC_SETUP:
    //  fn = mc_setup;
    //  break;
    //case QPU_MC_FILTER:
    //  fn = mc_filter;
    //  break;
    case QPU_MC_EXIT:
      fn = mc_exit;
      break;
    //case QPU_MC_INTERRUPT_EXIT:
    //  fn = mc_interrupt_exit;
    //  break;
    //case QPU_MC_FILTER_B:
    //  fn = mc_filter_b;
    //  break;
    //case QPU_MC_FILTER_HONLY:
    //  fn = mc_filter_honly;
    //  break;
    case QPU_MC_SETUP_UV:
      fn = mc_setup_uv;
      break;
    case QPU_MC_FILTER_UV:
      fn = mc_filter_uv;
      break;
    case QPU_MC_FILTER_UV_B0:
      fn = mc_filter_uv_b0;
      break;
    case QPU_MC_FILTER_UV_B:
      fn = mc_filter_uv_b;
      break;
    case QPU_MC_INTERRUPT_EXIT8:
      fn = mc_interrupt_exit8;
      break;
    case QPU_MC_END:
      fn = mc_end;
      break;
    default:
      printf("Unknown function\n");
      exit(-1);
    }
    return gpu->vc + 4*(int)(fn-rpi_shader);
    //return code[num] + gpu->vc;
}

#if 0
typedef unsigned int uint32_t;

typedef struct mvs_s {
    GPU_MEM_PTR_T unif_mvs_ptr;
    uint32_t *unif_mvs; // Base of memory for motion vector commands
    
    // _base pointers are to the start of the row
    uint32_t *mvs_base[8];
    // these pointers are to the next free space
    uint32_t *u_mvs[8];

} HEVCContext;

#define RPI_CHROMA_COMMAND_WORDS 12

static void rpi_inter_clear(HEVCContext *s)
{
    int i;
    for(i=0;i<8;i++) {
        s->u_mvs[i] = s->mvs_base[i];
        *s->u_mvs[i]++ = 0;
        *s->u_mvs[i]++ = 0;
        *s->u_mvs[i]++ = 0;
        *s->u_mvs[i]++ = 0;
        *s->u_mvs[i]++ = 0;
        *s->u_mvs[i]++ = 128;  // w
        *s->u_mvs[i]++ = 128;  // h
        *s->u_mvs[i]++ = 128;  // stride u
        *s->u_mvs[i]++ = 128;  // stride v
        s->u_mvs[i] += 3;  // Padding words
    }
}

static void rpi_execute_inter_qpu(HEVCContext *s)
{
    int k;
    uint32_t *unif_vc = (uint32_t *)s->unif_mvs_ptr.vc;   
                    
    for(k=0;k<8;k++) {
        s->u_mvs[k][-RPI_CHROMA_COMMAND_WORDS] = qpu_get_fn(QPU_MC_EXIT); // Add exit command
        s->u_mvs[k][-RPI_CHROMA_COMMAND_WORDS+3] = qpu_get_fn(QPU_MC_SETUP); // A dummy texture location (maps to our code) - this is needed as the texture requests are pipelined     
        s->u_mvs[k][-RPI_CHROMA_COMMAND_WORDS+4] = qpu_get_fn(QPU_MC_SETUP); //  dummy location for V    
    }
             
    s->u_mvs[8-1][-RPI_CHROMA_COMMAND_WORDS] = qpu_get_fn(QPU_MC_INTERRUPT_EXIT8); // This QPU will signal interrupt when all others are done and have acquired a semaphore

    qpu_run_shader8(qpu_get_fn(QPU_MC_SETUP_UV),
      (uint32_t)(unif_vc+(s->mvs_base[0 ] - (uint32_t*)s->unif_mvs_ptr.arm)),
      (uint32_t)(unif_vc+(s->mvs_base[1 ] - (uint32_t*)s->unif_mvs_ptr.arm)),
      (uint32_t)(unif_vc+(s->mvs_base[2 ] - (uint32_t*)s->unif_mvs_ptr.arm)),
      (uint32_t)(unif_vc+(s->mvs_base[3 ] - (uint32_t*)s->unif_mvs_ptr.arm)),
      (uint32_t)(unif_vc+(s->mvs_base[4 ] - (uint32_t*)s->unif_mvs_ptr.arm)),
      (uint32_t)(unif_vc+(s->mvs_base[5 ] - (uint32_t*)s->unif_mvs_ptr.arm)),
      (uint32_t)(unif_vc+(s->mvs_base[6 ] - (uint32_t*)s->unif_mvs_ptr.arm)),
      (uint32_t)(unif_vc+(s->mvs_base[7 ] - (uint32_t*)s->unif_mvs_ptr.arm))
      );
}

void rpi_test_qpu(void)
{
    HEVCContext mvs;
    HEVCContext *s = &mvs;
    int i;
    int uv_commands_per_qpu = (1 + (256*64*2)/(4*4)) * RPI_CHROMA_COMMAND_WORDS;
    uint32_t *p;
    printf("Allocate memory\n");
    gpu_malloc_uncached( 8 * uv_commands_per_qpu * sizeof(uint32_t), &s->unif_mvs_ptr );
    s->unif_mvs = (uint32_t *) s->unif_mvs_ptr.arm;

    // Set up initial locations for uniform streams
    p = s->unif_mvs;
    for(i = 0; i < 8; i++) {
        s->mvs_base[i] = p;
        p += uv_commands_per_qpu;
    }
    // Now run a simple program that should just quit immediately after a single texture fetch
    rpi_inter_clear(s);
    for(i=0;i<4;i++) {
      printf("Launch QPUs\n");
      rpi_execute_inter_qpu(s);
      printf("Done\n");
    }
    printf("Free memory\n");
    gpu_free(&s->unif_mvs_ptr);
    return;
}
#endif

#if 0

int32_t hcoeffs[] = {-4, 10, -21, 70, 90, -24, 11, -4};
//int32_t hcoeffs[] = {1, 1, 1, 1, 1, 1, 1, 1};
int32_t vcoeffs[] = {-2, 6, -13, 37, 115, -20, 9, -4};
//int32_t vcoeffs[] = {1, 1, 1, 1, 1, 1, 1, 1};

#define ENCODE_COEFFS(c0, c1, c2, c3) (((c0-1) & 0xff) | ((c1-1) & 0xff) << 8 | ((c2-1) & 0xff) << 16 | ((c3-1) & 0xff) << 24);

static uint8_t av_clip_uint8(int32_t a)
{
    if (a&(~255)) return (-a)>>31;
    else          return a;
}

static int32_t filter8(const uint8_t *data, int pitch)
{
   int32_t vsum = 0;
   int x, y;

   for (y = 0; y < 8; y++) {
      int32_t hsum = 0;

      for (x = 0; x < 8; x++)
         hsum += hcoeffs[x]*data[x + y * pitch];

      vsum += vcoeffs[y]*av_clip_uint8( (hsum + 64) >> 7); // Added brackets to stop compiler warning
   }

   return av_clip_uint8( (vsum + 64) >> 7);
}

// Note regression changes coefficients so is not thread safe
//#define REGRESSION
#ifdef REGRESSION
#define CMAX 100
#else
#define CMAX 2
#endif
#define YMAX 16

int rpi_test_shader(void)
{
   int i, c;

   uint32_t *unifs;

   uint8_t *in_buffer;
   uint8_t *out_buffer[2];
   
   GPU_MEM_PTR_T unifs_ptr;
   GPU_MEM_PTR_T in_buffer_ptr;
   GPU_MEM_PTR_T out_buffer_ptr[2];
   
   // Addresses in GPU memory of filter programs
   uint32_t mc_setup = 0;
   uint32_t mc_filter = 0;
   uint32_t mc_exit = 0;
   
   int pitch = 0x500;

   if (gpu==NULL) {
      gpu_lock();
      gpu_unlock();
   }
   
   printf("This needs to change to reflect new assembler\n");
   // Use table to compute locations of program start points
   mc_setup = code[0] + gpu->vc;
   mc_filter = code[1] + gpu->vc;
   mc_exit = code[2] + gpu->vc;

   if (!vcos_verify(gpu_malloc_uncached(4*64,&unifs_ptr))) {
      return -2;
   }
   unifs = (uint32_t*)unifs_ptr.arm;

   if (!vcos_verify(gpu_malloc_uncached(64*23,&in_buffer_ptr))) {
      return -3;
   }
   in_buffer = (uint8_t*)in_buffer_ptr.arm;

   if (!vcos_verify(gpu_malloc_uncached(16*pitch,&out_buffer_ptr[0])) || !vcos_verify(gpu_malloc_uncached(16*pitch,&out_buffer_ptr[1]))) {
      return -4;
   }
   out_buffer[0] = (uint8_t*)out_buffer_ptr[0].arm;
   out_buffer[1] = (uint8_t*)out_buffer_ptr[1].arm;

   for (c = 0; c < CMAX; c++) {
      int xo[] = {rand()&31, rand()&31};

#ifdef REGRESSION
      for (i = 0; i < 8; i++) {
         hcoeffs[i] = (int8_t)rand();
         vcoeffs[i] = (int8_t)rand();
         if (hcoeffs[i]==-128)
           hcoeffs[i]++;
         if (vcoeffs[i]==-128)
           vcoeffs[i]++;
      }
#endif

      for (i = 0; i < 64*23; i++) {
         //printf("%d %d %p\n",i,gpu->mb,&in_buffer[i]);
         in_buffer[i] = rand();
      }
           
      // Clear output array
      { 
        int b;
        for(b=0;b<2;b++) {
          for(i=0;i<16*16;i++) {
            out_buffer[b][i] = 3;
          }
        }
      }

      unifs[0] = mc_filter;
      unifs[1] = in_buffer_ptr.vc+xo[0]+16;
      unifs[2] = 64; // src pitch
      unifs[3] = pitch; // dst pitch
      unifs[4] = 0; // Padding
      unifs[5] = 0;
      unifs[6] = 0;
      unifs[7 ] = mc_filter;
      unifs[8 ] = in_buffer_ptr.vc+xo[1]+16;
      unifs[9 ] = ENCODE_COEFFS(hcoeffs[0], hcoeffs[1], hcoeffs[2], hcoeffs[3]);
      unifs[10] = ENCODE_COEFFS(hcoeffs[4], hcoeffs[5], hcoeffs[6], hcoeffs[7]);
      unifs[11] = ENCODE_COEFFS(vcoeffs[0], vcoeffs[1], vcoeffs[2], vcoeffs[3]);
      unifs[12] = ENCODE_COEFFS(vcoeffs[4], vcoeffs[5], vcoeffs[6], vcoeffs[7]);
      unifs[13] = out_buffer_ptr[0].vc;
      unifs[14] = mc_exit;
      unifs[15] = in_buffer_ptr.vc+xo[1]+16;        // dummy
      unifs[16] = ENCODE_COEFFS(hcoeffs[0], hcoeffs[1], hcoeffs[2], hcoeffs[3]);
      unifs[17] = ENCODE_COEFFS(hcoeffs[4], hcoeffs[5], hcoeffs[6], hcoeffs[7]);
      unifs[18] = ENCODE_COEFFS(vcoeffs[0], vcoeffs[1], vcoeffs[2], vcoeffs[3]);
      unifs[19] = ENCODE_COEFFS(vcoeffs[4], vcoeffs[5], vcoeffs[6], vcoeffs[7]);
      unifs[20] = out_buffer_ptr[1].vc;
      
      printf("Gpu->vc=%x Code=%x dst=%x\n",gpu->vc, mc_filter,out_buffer_ptr[1].vc);

      // flush_dcache(); TODO is this needed on ARM side? - tried to use the direct alias to avoid this problem

      //qpu_run_shader(mc_setup, unifs_ptr.vc);
      //qpu_run_shader(gpu, gpu->vc, unifs_ptr.vc);
      rpi_do_block(in_buffer_ptr.vc+xo[0]+16, 64, out_buffer_ptr[0].vc, pitch,out_buffer[0]);
      rpi_do_block(in_buffer_ptr.vc+xo[1]+16, 64, out_buffer_ptr[1].vc, pitch,out_buffer[1]);

      if (1)
      {
         int x, y, b;
         int bad = 0;

         for (b=0; b<2; ++b)
            for (y=0; y<YMAX; ++y)
               for (x=0; x<16; ++x) {
                  int32_t ref = filter8(in_buffer+x+y*64+xo[b], 64);

                  if (out_buffer[b][x+y*pitch] != ref) {
                      bad = 1;
//                     printf("%d, %d, %d, %d\n", c, b, x, y);
                  }
#ifndef REGRESSION
                  //printf("%08x %08x\n", out_buffer[b][x+y*pitch], ref);
#endif
               }
          if (bad)
            printf("Failed dst=%x test=%d\n",out_buffer_ptr[1].vc,c);
          else
            printf("Passed dst=%x test=%d\n",out_buffer_ptr[1].vc,c);
      }
      //printf("%d\n", simpenrose_get_qpu_tick_count());
   }

   gpu_free(&out_buffer_ptr[0]);
   gpu_free(&out_buffer_ptr[1]);
   gpu_free(&in_buffer_ptr);
   gpu_free(&unifs_ptr);

   return 0;
}

void rpi_do_block_arm(const uint8_t *in_buffer, int src_pitch, uint8_t *dst, int dst_pitch)
{
  int x,y;
  for (y=0; y<16; ++y) {
    for (x=0; x<16; ++x) {
       dst[x+y*dst_pitch] = filter8(in_buffer+x+y*src_pitch, src_pitch);
    }
  }
}

void rpi_do_block(const uint8_t *in_buffer_vc, int src_pitch, uint8_t *dst_vc, int dst_pitch, uint8_t *dst)
{
   uint32_t *unifs;

   GPU_MEM_PTR_T unifs_ptr;
   //uint8_t *out_buffer;
   //GPU_MEM_PTR_T out_buffer_ptr;
   
   // Addresses in GPU memory of filter programs
   uint32_t mc_setup = 0;
   uint32_t mc_filter = 0;
   uint32_t mc_exit = 0;
   //int x,y;
    
   if (gpu==NULL) {
      gpu_lock();
      gpu_unlock();
   }
   
   // Use table to compute locations of program start points
   mc_setup = code[0] + gpu->vc;
   mc_filter = code[1] + gpu->vc;
   mc_exit = code[2] + gpu->vc;
   
   if (!vcos_verify(gpu_malloc_uncached(4*64,&unifs_ptr))) {
      return;
   }
   //gpu_malloc_uncached(16*dst_pitch,&out_buffer_ptr);
   //out_buffer = (uint8_t*)out_buffer_ptr.arm;
   
   /*for (y=0; y<16; ++y) {
      for (x=0; x<16; ++x) {
         out_buffer[x+y*dst_pitch] = 7;
      }
    }*/
   
   unifs = (uint32_t*)unifs_ptr.arm;
   
    unifs[0] = mc_filter;
    unifs[1] = (int)in_buffer_vc;
    unifs[2] = src_pitch; // src pitch
    unifs[3] = dst_pitch; // dst pitch
    unifs[4] = 0; // Padding
    unifs[5] = 0;
    unifs[6] = 0;
    unifs[7 ] = mc_exit;
    unifs[8 ] = (int)in_buffer_vc;
    unifs[9 ] = ENCODE_COEFFS(hcoeffs[0], hcoeffs[1], hcoeffs[2], hcoeffs[3]);
    unifs[10] = ENCODE_COEFFS(hcoeffs[4], hcoeffs[5], hcoeffs[6], hcoeffs[7]);
    unifs[11] = ENCODE_COEFFS(vcoeffs[0], vcoeffs[1], vcoeffs[2], vcoeffs[3]);
    unifs[12] = ENCODE_COEFFS(vcoeffs[4], vcoeffs[5], vcoeffs[6], vcoeffs[7]);
    unifs[13] = (int)dst_vc;
    //unifs[13] = (int)out_buffer_ptr.vc;

    //printf("Gpu->vc=%x Code=%x dst=%x\n",gpu->vc, mc_filter,out_buffer_ptr[1].vc);

    qpu_run_shader(mc_setup, unifs_ptr.vc);
    
    /*for (y=0; y<16; ++y) {
      for (x=0; x<16; ++x) {
         dst[x+y*dst_pitch] = out_buffer[x+y*dst_pitch];
      }
    }*/
    
    gpu_free(&unifs_ptr);
    //gpu_free(&out_buffer_ptr);
}



#endif
