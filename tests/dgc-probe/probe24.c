/*
 * probe24 - the cost of one implicit (preprocess-on-execute)
 * vkCmdExecuteGeneratedCommandsEXT inside a series of executes.
 *
 * probe21 puts a single execute in a submission, so its total carries the
 * submit, the render pass and the barrier together and says nothing about
 * what the second execute of a frame costs. Here one command buffer runs M
 * executes of the same layout back to back, which is what vkd3d records when
 * an application calls ExecuteIndirect in a loop. The cost of one more
 * execute is everything a batcher of consecutive executes would remove: the
 * preprocess dispatch, its barrier and the state emitted around it.
 *
 * Reported, in microseconds:
 *
 *   one  - the whole submission with a single execute: fixed cost plus the
 *          sequences;
 *   exec - the marginal cost of one more execute,
 *          (total(M) - total(1)) / (M - 1);
 *   man  - the same marginal cost for the manual CPU loop the driver runs
 *          when there is no DGC.
 *
 * The GPU work per sequence is a triangle of near-zero area, so the marginal
 * cost is command processing, not fill.
 */
#include "pcommon.h"
#include "vs_full_spv.h"
#include "fs_green_spv.h"
#include "vs_pc_spv.h"

#define PC_BYTES 16
#define M_EXEC 33

static PFN_vkCreateIndirectCommandsLayoutEXT pCreateLayout;
static PFN_vkGetGeneratedCommandsMemoryRequirementsEXT pMemReq;
static PFN_vkCmdExecuteGeneratedCommandsEXT pExecute;

enum kind { V_DRAW, V_PC_DRAW, V_INDEXED };

struct seq_draw {
   uint32_t vertex_count, instance_count, first_vertex, first_instance;
};
struct seq_pc {
   uint32_t pc[PC_BYTES / 4];
   struct seq_draw draw;
};
struct seq_idx {
   uint64_t index_addr;
   uint32_t index_size_B, index_format;
   uint32_t index_count, instance_count, first_index, vertex_offset,
      first_instance;
};

struct variant {
   const char *name;
   enum kind kind;
   size_t stride;
   VkPipeline pipe;
   VkPipelineLayout pl;
   VkIndirectCommandsLayoutEXT lay;
   VkDeviceAddress seq_va;
   VkBuffer ib;
};

struct meas {
   uint64_t inner[64], total[64];
   int n;
};

/* The marginal cost is a difference of two totals, so the noise of the
 * slower of the two shows up doubled and with a sign. The fastest submission
 * of a series is the one the GPU was least contended for, which is the number
 * that should be compared. */
static uint64_t
min_u64(const uint64_t *v, int n)
{
   uint64_t m = UINT64_MAX;
   for (int i = 0; i < n; i++)
      if (v[i] < m)
         m = v[i];
   return m;
}

static uint64_t
submit_wait_ns(struct env *e)
{
   uint64_t t0 = now_ns();
   VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                       .commandBufferCount = 1,
                       .pCommandBuffers = &e->cmd };
   VKCHECK(vkQueueSubmit(e->queue, 1, &si, e->fence));
   VKCHECK(vkWaitForFences(e->dev, 1, &e->fence, VK_TRUE, UINT64_MAX));
   VKCHECK(vkResetFences(e->dev, 1, &e->fence));
   VKCHECK(vkResetCommandBuffer(e->cmd, 0));
   return now_ns() - t0;
}

/* Implicit layouts: no EXPLICIT_PREPROCESS flag, so every execute runs the
 * preprocess shader. This is the shape vkd3d uses for ExecuteIndirect. */
static VkIndirectCommandsLayoutEXT
make_layout(struct env *e, enum kind kind, uint32_t stride, VkPipelineLayout pl)
{
   VkIndirectCommandsPushConstantTokenEXT pct = {
      .updateRange = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0,
                       .size = PC_BYTES }
   };
   VkIndirectCommandsIndexBufferTokenEXT ibt = {
      .mode = VK_INDIRECT_COMMANDS_INPUT_MODE_DXGI_INDEX_BUFFER_EXT
   };
   VkIndirectCommandsLayoutTokenEXT toks[2];
   uint32_t n = 1;

   switch (kind) {
   case V_DRAW:
      toks[0] = (VkIndirectCommandsLayoutTokenEXT){
         .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,
         .type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT, .offset = 0
      };
      break;
   case V_PC_DRAW:
      toks[0] = (VkIndirectCommandsLayoutTokenEXT){
         .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,
         .type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_CONSTANT_EXT,
         .offset = 0, .data.pPushConstant = &pct
      };
      toks[1] = (VkIndirectCommandsLayoutTokenEXT){
         .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,
         .type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT, .offset = PC_BYTES
      };
      n = 2;
      break;
   case V_INDEXED:
      toks[0] = (VkIndirectCommandsLayoutTokenEXT){
         .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,
         .type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_INDEX_BUFFER_EXT, .offset = 0,
         .data.pIndexBuffer = &ibt
      };
      toks[1] = (VkIndirectCommandsLayoutTokenEXT){
         .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,
         .type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_EXT, .offset = 16
      };
      n = 2;
      break;
   }

   VkIndirectCommandsLayoutCreateInfoEXT lci = {
      .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT,
      .shaderStages = VK_SHADER_STAGE_ALL_GRAPHICS, .indirectStride = stride,
      .pipelineLayout = pl, .tokenCount = n, .pTokens = toks
   };
   VkIndirectCommandsLayoutEXT lay;
   VKCHECK(pCreateLayout(e->dev, &lci, NULL, &lay));
   return lay;
}

/* manual is the loop the driver would record without DGC; otherwise the
 * window holds count executes of the layout. */
static struct meas
measure(struct env *e, struct variant *v, uint32_t nseq, uint32_t count,
         bool manual, int reps)
{
   struct meas m = { 0 };
   VkGeneratedCommandsPipelineInfoEXT pinfo = {
      .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT,
      .pipeline = v->pipe
   };
   VkGeneratedCommandsInfoEXT gi = {
      .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT, .pNext = &pinfo,
      .shaderStages = VK_SHADER_STAGE_ALL_GRAPHICS,
      .indirectCommandsLayout = v->lay, .indirectAddress = v->seq_va,
      .indirectAddressSize = (VkDeviceSize) v->stride * nseq,
      .maxSequenceCount = nseq, .maxDrawCount = nseq
   };
   uint32_t pc[PC_BYTES / 4] = { 0x11223344, 1, 2, 3 };

   for (int r = 0; r < reps; r++) {
      VkCommandBufferBeginInfo cbi = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
      };
      VKCHECK(vkBeginCommandBuffer(e->cmd, &cbi));
      vkCmdBindPipeline(e->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v->pipe);
      begin_renderpass(e, C_MAGENTA);
      VkDeviceSize zero = 0;
      vkCmdBindVertexBuffers(e->cmd, 0, 1, &e->vbo, &zero);
      if (v->kind == V_INDEXED)
         vkCmdBindIndexBuffer(e->cmd, v->ib, 0, VK_INDEX_TYPE_UINT16);

      uint64_t i0 = now_ns(), i1;
      for (uint32_t k = 0; k < count; k++) {
         if (manual) {
            for (uint32_t i = 0; i < nseq; i++) {
               if (v->kind == V_PC_DRAW)
                  vkCmdPushConstants(e->cmd, v->pl, VK_SHADER_STAGE_VERTEX_BIT,
                                     0, PC_BYTES, pc);
               if (v->kind == V_INDEXED)
                  vkCmdDrawIndexed(e->cmd, 3, 1, 0, 0, 0);
               else
                  vkCmdDraw(e->cmd, 3, 1, 0, 0);
            }
         } else {
            pExecute(e->cmd, VK_FALSE, &gi);
         }
      }
      i1 = now_ns();

      end_renderpass(e);
      VKCHECK(vkEndCommandBuffer(e->cmd));
      uint64_t tw = submit_wait_ns(e);
      m.inner[m.n] = i1 - i0;
      m.total[m.n] = (i1 - i0) + tw;
      m.n++;
   }
   return m;
}

static void
verify(struct env *e, struct variant *v)
{
   const uint32_t nseq = 4;
   VkGeneratedCommandsPipelineInfoEXT pinfo = {
      .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT,
      .pipeline = v->pipe
   };
   VkGeneratedCommandsInfoEXT gi = {
      .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT, .pNext = &pinfo,
      .shaderStages = VK_SHADER_STAGE_ALL_GRAPHICS,
      .indirectCommandsLayout = v->lay, .indirectAddress = v->seq_va,
      .indirectAddressSize = (VkDeviceSize) v->stride * nseq,
      .maxSequenceCount = nseq, .maxDrawCount = nseq
   };
   VkCommandBufferBeginInfo cbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
   };
   VKCHECK(vkBeginCommandBuffer(e->cmd, &cbi));
   vkCmdBindPipeline(e->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v->pipe);
   begin_renderpass(e, C_MAGENTA);
   VkDeviceSize zero = 0;
   vkCmdBindVertexBuffers(e->cmd, 0, 1, &e->vbo, &zero);
   if (v->kind == V_INDEXED)
      vkCmdBindIndexBuffer(e->cmd, v->ib, 0, VK_INDEX_TYPE_UINT16);
   /* Twice in a row: the second execute reuses the stream ring and must not
    * see the first one's bytes. */
   pExecute(e->cmd, VK_FALSE, &gi);
   pExecute(e->cmd, VK_FALSE, &gi);
   end_renderpass(e);
   VKCHECK(vkEndCommandBuffer(e->cmd));
   submit_wait_ns(e);

   void *map = readback(e);
   char tag[64];
   snprintf(tag, sizeof(tag), "%s/series", v->name);
   check(e, tag, W / 2, H / 2, px(map, W / 2, H / 2), C_GREEN);
}

int
main(void)
{
   struct env e;
   struct env_ext ext = { .dgc = VK_TRUE };
   env_init_ex(&e, "probe24", &ext);
   pCreateLayout = (PFN_vkCreateIndirectCommandsLayoutEXT) vkGetDeviceProcAddr(
      e.dev, "vkCreateIndirectCommandsLayoutEXT");
   pMemReq = (PFN_vkGetGeneratedCommandsMemoryRequirementsEXT)
                vkGetDeviceProcAddr(e.dev,
                                    "vkGetGeneratedCommandsMemoryRequirementsEXT");
   pExecute = (PFN_vkCmdExecuteGeneratedCommandsEXT) vkGetDeviceProcAddr(
      e.dev, "vkCmdExecuteGeneratedCommandsEXT");
   if (!pCreateLayout || !pMemReq || !pExecute)
      return 3;

   struct stagex st_plain[2] = { { VK_SHADER_STAGE_VERTEX_BIT, VS_FULL_SPV,
                                   VS_FULL_SPV_N * 4 },
                                 { VK_SHADER_STAGE_FRAGMENT_BIT, FS_GREEN_SPV,
                                   FS_GREEN_SPV_N * 4 } };
   struct stagex st_pc[2] = { { VK_SHADER_STAGE_VERTEX_BIT, VS_PC_SPV,
                                VS_PC_SPV_N * 4 },
                              { VK_SHADER_STAGE_FRAGMENT_BIT, FS_GREEN_SPV,
                                FS_GREEN_SPV_N * 4 } };
   VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
                               .offset = 0, .size = PC_BYTES };
   VkPipelineLayout pl_plain, pl_pc;
   VkPipeline pipe_plain =
      make_pipeline_ex(&e, st_plain, 2, NULL, 0, NULL, &pl_plain);
   VkPipeline pipe_pc = make_pipeline_ex(&e, st_pc, 2, NULL, 0, &pcr, &pl_pc);

   /* A triangle of almost no area: the point is command processing, not
    * fill. */
   float verts[3][2] = { { -0.008f, 0.008f }, { 0.008f, 0.008f },
                         { 0.0f, -0.016f } };
   buf_create(&e, &e.vbo, &e.vmem, sizeof(verts),
              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 1);
   buf_write(&e, e.vmem, verts, sizeof(verts));

   uint16_t ib[16] = { 0, 1, 2 };
   VkBuffer ibo;
   VkDeviceMemory ibom;
   bda_usage_buffer(&e, &ibo, &ibom, sizeof(ib), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
   buf_write(&e, ibom, ib, sizeof(ib));
   VkBufferDeviceAddressInfo ibai = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = ibo
   };
   VkDeviceAddress ib_va = vkGetBufferDeviceAddress(e.dev, &ibai);

   const uint32_t NMAX = 64;
   struct variant v[3] = {
      { "draw", V_DRAW, sizeof(struct seq_draw), pipe_plain, pl_plain },
      { "pc_draw", V_PC_DRAW, sizeof(struct seq_pc), pipe_pc, pl_pc },
      { "indexed", V_INDEXED, sizeof(struct seq_idx), pipe_plain, pl_plain },
   };

   for (int i = 0; i < 3; i++) {
      v[i].ib = ibo;
      VkBuffer sb;
      VkDeviceMemory sm;
      VkDeviceSize sz = (VkDeviceSize) v[i].stride * NMAX;
      uint8_t *seqs = calloc(NMAX, v[i].stride);
      for (uint32_t k = 0; k < NMAX; k++) {
         uint8_t *p = seqs + k * v[i].stride;
         if (v[i].kind == V_DRAW) {
            struct seq_draw d = { 3, 1, 0, 0 };
            memcpy(p, &d, sizeof(d));
         } else if (v[i].kind == V_PC_DRAW) {
            struct seq_pc s = { { 0x1000 + k, 1, 2, 3 }, { 3, 1, 0, 0 } };
            memcpy(p, &s, sizeof(s));
         } else {
            struct seq_idx s = { .index_addr = ib_va,
                                 .index_size_B = sizeof(ib),
                                 .index_format = 57 /* DXGI_FORMAT_R16_UINT */,
                                 .index_count = 3, .instance_count = 1 };
            memcpy(p, &s, sizeof(s));
         }
      }
      v[i].seq_va = bda_usage_buffer(&e, &sb, &sm, sz,
                                     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      buf_write(&e, sm, seqs, sz);
      free(seqs);
      v[i].lay = make_layout(&e, v[i].kind, (uint32_t) v[i].stride, v[i].pl);
   }

   for (int i = 0; i < 3; i++)
      verify(&e, &v[i]);

   printf("\n%-8s %4s | %8s %8s %8s | %8s %8s %8s | ratios\n", "variant", "N",
          "dgc one", "dgc exec", "dgc in", "man one", "man exec", "man in");
   static const uint32_t NS[] = { 1, 16, 64 };
   for (int i = 0; i < 3; i++) {
      for (unsigned j = 0; j < sizeof(NS) / sizeof(NS[0]); j++) {
         uint32_t n = NS[j];
         int reps = n <= 1 ? 11 : 7;

         struct meas d1 = measure(&e, &v[i], n, 1, false, reps);
         struct meas dm = measure(&e, &v[i], n, M_EXEC, false, reps);
         struct meas m1 = measure(&e, &v[i], n, 1, true, reps);
         struct meas mm = measure(&e, &v[i], n, M_EXEC, true, reps);

         uint64_t d1t = min_u64(d1.total, d1.n);
         uint64_t dmt = min_u64(dm.total, dm.n);
         uint64_t m1t = min_u64(m1.total, m1.n);
         uint64_t mmt = min_u64(mm.total, mm.n);
         double dexec =
            (double) ((int64_t) dmt - (int64_t) d1t) / (M_EXEC - 1) / 1000.0;
         double mexec =
            (double) ((int64_t) mmt - (int64_t) m1t) / (M_EXEC - 1) / 1000.0;

         printf("%-8s %4u | %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | dgc/man "
                "%.2f\n",
                v[i].name, n, us(d1t), dexec, us(median_u64(d1.inner, d1.n)),
                us(m1t), mexec, us(median_u64(m1.inner, m1.n)),
                mexec > 0.0 ? dexec / mexec : 0.0);
      }
   }

   printf("probe24: %d checks, %d fail\n", e.checks, e.fails);
   return e.fails ? 1 : 0;
}
