#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/msm_drm.h>
/* prefer /usr/include/drm/msm_drm.h in build cmd */

#define A6XX_CP_SCRATCH_REG6 0x630

static int drm_fd = -1;

static int do_ioctl(unsigned long req, void *arg) {
  int ret = ioctl(drm_fd, req, arg);
  if (ret < 0) return -errno;
  return ret;
}

static uint32_t gem_new(uint64_t size, uint32_t flags) {
  struct drm_msm_gem_new req = {.size = size, .flags = flags};
  int ret = do_ioctl(DRM_IOCTL_MSM_GEM_NEW, &req);
  if (ret) { fprintf(stderr, "GEM_NEW: %s\n", strerror(-ret)); exit(2); }
  return req.handle;
}

#ifndef MSM_INFO_GET_IOVA
#define MSM_INFO_GET_IOVA 0x01
#endif
#ifndef MSM_INFO_GET_OFFSET
#define MSM_INFO_GET_OFFSET 0x02
#endif
static uint64_t gem_info_iova(uint32_t handle) {
  struct drm_msm_gem_info req = {.handle = handle, .info = MSM_INFO_GET_IOVA};
  int ret = do_ioctl(DRM_IOCTL_MSM_GEM_INFO, &req);
  if (ret) { fprintf(stderr, "GEM_INFO: %s\n", strerror(-ret)); exit(2); }
  return req.value;
}

static void *gem_mmap(uint32_t handle, uint64_t size) {
  struct drm_msm_gem_info req = {.handle = handle, .info = MSM_INFO_GET_OFFSET};
  int ret = do_ioctl(DRM_IOCTL_MSM_GEM_INFO, &req);
  if (ret) { fprintf(stderr, "GEM_INFO mmap: %s\n", strerror(-ret)); exit(2); }
  void *p = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, drm_fd, req.value);
  if (p == MAP_FAILED) { perror("mmap"); exit(2); }
  return p;
}

static void gem_close(uint32_t handle) {
  struct drm_gem_close req = {.handle = handle};
  do_ioctl(DRM_IOCTL_GEM_CLOSE, &req);
}

static uint32_t submitqueue_new(uint32_t prio) {
  struct drm_msm_submitqueue req = {.flags = 0, .prio = prio};
  int ret = do_ioctl(DRM_IOCTL_MSM_SUBMITQUEUE_NEW, &req);
  if (ret) { fprintf(stderr, "SUBMITQUEUE_NEW: %s\n", strerror(-ret)); exit(2); }
  return req.id;
}

static void submitqueue_close(uint32_t id) {
  do_ioctl(DRM_IOCTL_MSM_SUBMITQUEUE_CLOSE, &id);
}

static uint32_t gpu_id(void) {
  struct drm_msm_param req = {.pipe = MSM_PIPE_3D0, .param = MSM_PARAM_GPU_ID};
  int ret = do_ioctl(DRM_IOCTL_MSM_GET_PARAM, &req);
  if (ret) { fprintf(stderr, "GET_PARAM: %s\n", strerror(-ret)); exit(2); }
  return (uint32_t)req.value;
}

static inline uint32_t pm4_pkt7(uint8_t opcode, uint16_t cnt) {
  return (3u << 30) | ((uint32_t)cnt << 16) | ((uint32_t)opcode << 8);
}
enum { CP_NOP=0x10, CP_WAIT_MEM_WRITES=0x12, CP_MEM_WRITE=0x3d, CP_MEM_TO_REG=0x42, CP_COND_REG_EXEC=0x47, CP_COND_EXEC=0x47 /* no, wait */ };
#undef CP_COND_EXEC
enum { CP_COND_EXEC_OP = 0x44 };

struct bo {
  uint32_t handle;
  uint64_t size;
  uint64_t iova;
  void *map;
};

static struct bo bo_new(uint64_t size, uint32_t flags) {
  struct bo b = {.size = size};
  b.handle = gem_new(size, flags);
  b.iova = gem_info_iova(b.handle);
  b.map = gem_mmap(b.handle, size);
  memset(b.map, 0, size);
  return b;
}
static void bo_destroy(struct bo *b) {
  if (b->map) munmap(b->map, b->size);
  if (b->handle) gem_close(b->handle);
  memset(b, 0, sizeof(*b));
}

static int submit(uint32_t queue_id, struct bo *cmd_bo, uint32_t cmd_dw, struct bo *data_bo) {
  struct drm_msm_gem_submit_bo bos[2] = {
    {.handle = cmd_bo->handle, .flags = MSM_SUBMIT_BO_READ},
    {.handle = data_bo->handle, .flags = MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_WRITE},
  };
  struct drm_msm_gem_submit_cmd cmd = {
    .type = MSM_SUBMIT_CMD_BUF,
    .submit_idx = 0,
    .submit_offset = 0,
    .size = cmd_dw * 4u,
  };
  struct drm_msm_gem_submit req = {
    .flags = MSM_PIPE_3D0,
    .queueid = queue_id,
    .nr_bos = 2,
    .nr_cmds = 1,
    .bos = (uint64_t)(uintptr_t)bos,
    .cmds = (uint64_t)(uintptr_t)&cmd,
  };
  int ret = do_ioctl(DRM_IOCTL_MSM_GEM_SUBMIT, &req);
  if (ret < 0) return ret;
  struct drm_msm_wait_fence wf = {.fence = req.fence, .queueid = queue_id};
  wf.timeout = (struct drm_msm_timespec){.tv_sec = 5, .tv_nsec = 0};
  ret = do_ioctl(DRM_IOCTL_MSM_WAIT_FENCE, &wf);
  return ret;
}

static void emit_mem_write(uint32_t *dw, unsigned *n, uint64_t dst, uint32_t val) {
  dw[(*n)++] = pm4_pkt7(CP_MEM_WRITE, 3);
  dw[(*n)++] = (uint32_t)dst;
  dw[(*n)++] = (uint32_t)(dst >> 32);
  dw[(*n)++] = val;
}

static int test_cond_reg_exec_imm(bool active) {
  struct bo data = bo_new(0x1000, MSM_BO_WC);
  struct bo cmd = bo_new(0x1000, MSM_BO_WC);
  volatile uint32_t *d = data.map;
  uint32_t *c = cmd.map;
  unsigned n = 0;

  /* d[0] = flag, d[1] = result */
  d[0] = active ? 1u : 0u;
  d[1] = 0xdeadbeefu;
  __sync_synchronize();

  uint64_t flag_va = data.iova + 0;
  uint64_t res_va  = data.iova + 4;
  uint32_t scratch = A6XX_CP_SCRATCH_REG6;

  /* CP_MEM_TO_REG: load flag into scratch reg */
  c[n++] = pm4_pkt7(CP_MEM_TO_REG, 3);
  c[n++] = (scratch & 0x3ffff) | (1u << 19); /* REG=scratch, CNT=1 */
  c[n++] = (uint32_t)flag_va;
  c[n++] = (uint32_t)(flag_va >> 32);

  /* CP_COND_REG_EXEC REG_COMPARE_IMM: if scratch == 0 skip 4 dwords (MEM_WRITE) */
  c[n++] = pm4_pkt7(CP_COND_REG_EXEC, 3);
  c[n++] = (scratch & 0x3ffff) | (4u << 28); /* REG0=scratch, MODE=REG_COMPARE_IMM */
  c[n++] = 0; /* IMM */
  c[n++] = 4; /* DWORDS to skip when condition true */

  /* MEM_WRITE result = 0x11223344 (4 dwords) */
  emit_mem_write(c, &n, res_va, 0x11223344u);

  /* Always write marker d[2]=0x55 */
  emit_mem_write(c, &n, data.iova + 8, 0x55u);

  c[n++] = pm4_pkt7(CP_NOP, 0);
  __sync_synchronize();

  uint32_t q = submitqueue_new(0);
  int ret = submit(q, &cmd, n, &data);
  submitqueue_close(q);

  uint32_t result = d[1];
  uint32_t marker = d[2];
  bo_destroy(&cmd);
  bo_destroy(&data);

  uint32_t expect = active ? 0x11223344u : 0xdeadbeefu;
  bool ok = ret == 0 && result == expect && marker == 0x55u;
  printf("cond_reg_exec_imm active=%d ret=%d result=0x%08x marker=0x%08x %s\n",
         active, ret, result, marker, ok ? "OK" : "FAIL");
  return ok ? 0 : 1;
}

static int test_cond_exec(bool active) {
  struct bo data = bo_new(0x1000, MSM_BO_WC);
  struct bo cmd = bo_new(0x1000, MSM_BO_WC);
  volatile uint32_t *d = data.map;
  uint32_t *c = cmd.map;
  unsigned n = 0;

  d[0] = active ? 1u : 0u; /* BOOL */
  d[1] = 0u;               /* TS */
  d[2] = 0xdeadbeefu;      /* result */
  __sync_synchronize();

  uint64_t bool_va = data.iova + 0;
  uint64_t ts_va   = data.iova + 4;
  uint64_t res_va  = data.iova + 8;

  /* CP_COND_EXEC: if BOOL != 0 && (ACTIVE_TS - TS) < 0x80000000, execute DWORDS */
  c[n++] = pm4_pkt7(CP_COND_EXEC_OP, 6);
  c[n++] = (uint32_t)bool_va;
  c[n++] = (uint32_t)(bool_va >> 32);
  c[n++] = (uint32_t)ts_va;
  c[n++] = (uint32_t)(ts_va >> 32);
  c[n++] = 0; /* ACTIVE_TIMESTAMP */
  c[n++] = 4; /* DWORDS: MEM_WRITE */

  emit_mem_write(c, &n, res_va, 0x11223344u);
  emit_mem_write(c, &n, data.iova + 12, 0x55u);
  c[n++] = pm4_pkt7(CP_NOP, 0);
  __sync_synchronize();

  uint32_t q = submitqueue_new(0);
  int ret = submit(q, &cmd, n, &data);
  submitqueue_close(q);

  uint32_t result = d[2];
  uint32_t marker = d[3];
  bo_destroy(&cmd);
  bo_destroy(&data);
  uint32_t expect = active ? 0x11223344u : 0xdeadbeefu;
  bool ok = ret == 0 && result == expect && marker == 0x55u;
  printf("cond_exec active=%d ret=%d result=0x%08x marker=0x%08x %s\n",
         active, ret, result, marker, ok ? "OK" : "FAIL");
  return ok ? 0 : 1;
}

int main(void) {
  drm_fd = open("/dev/dri/renderD128", O_RDWR);
  if (drm_fd < 0) { perror("open drm"); return 2; }
  printf("gpu_id=0x%x\n", gpu_id());
  int fail = 0;
  fail |= test_cond_reg_exec_imm(true);
  fail |= test_cond_reg_exec_imm(false);
  fail |= test_cond_exec(true);
  fail |= test_cond_exec(false);
  close(drm_fd);
  return fail ? 1 : 0;
}
