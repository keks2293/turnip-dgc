/*
 * probe26 - an executed stream longer than the hardware IB size limit.
 *
 * A CP_INDIRECT_BUFFER size field is 20 bits wide (tu_sanitize_ib_size() caps
 * IB sizes at 0x0fffff dwords), but the DGC stream is one IB of
 * maxSequenceCount * dwords_per_seq dwords, which can be far larger. RADV
 * bounds this with MAX_SEQUENCES_PER_IB and chains IBs at sequence boundaries.
 *
 * This probe makes the last sequence the only one that draws something
 * visible: every sequence pushes magic = 0 into the vertex shader, which
 * shifts the triangle off screen, and the last one pushes magic = 1. The
 * stride is 8 dwords and the count is 196608, so the stream is 0x180000
 * dwords; the low 20 bits are 0x80000 dwords = 65536 sequences, a whole
 * number of sequences. If the size is truncated there, the last sequence
 * never runs and the pixel stays at the clear colour.
 */
#include "pcommon.h"
#include "vs_pc_spv.h"
#include "fs_green_spv.h"

static PFN_vkCreateIndirectCommandsLayoutEXT pCreateLayout;
static PFN_vkGetGeneratedCommandsMemoryRequirementsEXT pMemReq;
static PFN_vkCmdExecuteGeneratedCommandsEXT pExecute;

#define PC_BYTES 16

struct seq_pc {
   uint32_t pc[PC_BYTES / 4];
   uint32_t vertex_count, instance_count, first_vertex, first_instance;
};

int main(void)
{
   struct env e;
   struct env_ext ext = { .dgc = VK_TRUE };
   env_init_ex(&e, "probe26", &ext);

   pCreateLayout = (PFN_vkCreateIndirectCommandsLayoutEXT)vkGetDeviceProcAddr(
      e.dev, "vkCreateIndirectCommandsLayoutEXT");
   pMemReq = (PFN_vkGetGeneratedCommandsMemoryRequirementsEXT)
      vkGetDeviceProcAddr(e.dev, "vkGetGeneratedCommandsMemoryRequirementsEXT");
   pExecute = (PFN_vkCmdExecuteGeneratedCommandsEXT)vkGetDeviceProcAddr(
      e.dev, "vkCmdExecuteGeneratedCommandsEXT");
   if (!pCreateLayout || !pMemReq || !pExecute)
      return 3;

   struct stagex st[2] = {
      { VK_SHADER_STAGE_VERTEX_BIT, VS_PC_SPV, VS_PC_SPV_N * 4 },
      { VK_SHADER_STAGE_FRAGMENT_BIT, FS_GREEN_SPV, FS_GREEN_SPV_N * 4 },
   };
   VkPushConstantRange pcr = {
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = PC_BYTES,
   };
   VkPipelineLayout pl;
   VkPipeline pipe = make_pipeline_ex(&e, st, 2, NULL, 0, &pcr, &pl);

   VkIndirectCommandsPushConstantTokenEXT pct = {
      .updateRange = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0,
                       .size = PC_BYTES },
   };
   VkIndirectCommandsLayoutTokenEXT tok[2] = {
      {
         .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,
         .type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_CONSTANT_EXT,
         .offset = 0,
         .data.pPushConstant = &pct,
      },
      {
         .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,
         .type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT,
         .offset = PC_BYTES,
      },
   };
   VkIndirectCommandsLayoutCreateInfoEXT lci = {
      .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT,
      .shaderStages = VK_SHADER_STAGE_ALL_GRAPHICS,
      .indirectStride = sizeof(struct seq_pc),
      .pipelineLayout = pl,
      .tokenCount = 2,
      .pTokens = tok,
   };
   VkIndirectCommandsLayoutEXT lay;
   VKCHECK(pCreateLayout(e.dev, &lci, NULL, &lay));

   const char *max_env = getenv("PROBE26_MAX");
   const uint32_t MAX_SEQ = max_env ? (uint32_t)strtoul(max_env, NULL, 0) : 196608u;
   const VkDeviceSize seqs_size = (VkDeviceSize)MAX_SEQ * sizeof(struct seq_pc);
   struct seq_pc *seqs = calloc(MAX_SEQ, sizeof(*seqs));
   for (uint32_t i = 0; i < MAX_SEQ; i++) {
      seqs[i].pc[0] = 0; /* magic == 0: the triangle is shifted off screen */
      seqs[i].vertex_count = 3;
      seqs[i].instance_count = 1;
   }
   seqs[MAX_SEQ - 1].pc[0] = 1; /* the only visible sequence */
   VkBuffer sb;
   VkDeviceMemory sm;
   VkDeviceAddress sva = bda_usage_buffer(
      &e, &sb, &sm, seqs_size,
      VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
   buf_write(&e, sm, seqs, seqs_size);
   free(seqs);

   uint32_t cnt = MAX_SEQ;
   VkBuffer cb;
   VkDeviceMemory cm;
   VkDeviceAddress cva = bda_usage_buffer(
      &e, &cb, &cm, sizeof(cnt),
      VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
   buf_write(&e, cm, &cnt, sizeof(cnt));

   VkBuffer ppb;
   VkDeviceMemory ppm;
   VkDeviceSize ppsz = 0;
   VkDeviceAddress ppva =
      preprocess_buffer(&e, pMemReq, lay, pipe, MAX_SEQ, &ppsz, &ppb, &ppm);

   VkGeneratedCommandsPipelineInfoEXT pi = {
      .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT,
      .pipeline = pipe,
   };
   VkGeneratedCommandsInfoEXT gi = {
      .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT,
      .pNext = &pi,
      .shaderStages = VK_SHADER_STAGE_ALL_GRAPHICS,
      .indirectCommandsLayout = lay,
      .indirectAddress = sva,
      .indirectAddressSize = seqs_size,
      .preprocessAddress = ppva,
      .preprocessSize = ppsz,
      .sequenceCountAddress = cva,
      .maxSequenceCount = MAX_SEQ,
      .maxDrawCount = MAX_SEQ,
   };

   float verts[3][2] = { { -0.008f, 0.008f }, { 0.008f, 0.008f },
                         { 0.0f, -0.016f } };
   buf_create(&e, &e.vbo, &e.vmem, sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 1);
   buf_write(&e, e.vmem, verts, sizeof(verts));

   VkCommandBufferBeginInfo cbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   VKCHECK(vkBeginCommandBuffer(e.cmd, &cbi));
   vkCmdBindPipeline(e.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   begin_renderpass(&e, C_MAGENTA);
   VkDeviceSize zero = 0;
   vkCmdBindVertexBuffers(e.cmd, 0, 1, &e.vbo, &zero);
   pExecute(e.cmd, VK_FALSE, &gi);
   end_renderpass(&e);
   VKCHECK(vkEndCommandBuffer(e.cmd));

   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &e.cmd,
   };
   VKCHECK(vkQueueSubmit(e.queue, 1, &si, e.fence));
   VKCHECK(vkWaitForFences(e.dev, 1, &e.fence, VK_TRUE, UINT64_MAX));
   VKCHECK(vkResetFences(e.dev, 1, &e.fence));

   void *map = readback(&e);
   fprintf(stderr, "probe26: stream %u sequences x %zu bytes = %llu bytes (%u dwords)\n",
           MAX_SEQ, sizeof(struct seq_pc),
           (unsigned long long)seqs_size,
           (unsigned)(seqs_size / 4));
   check(&e, "last of a >0x0fffff-dword stream", W / 2, H / 2,
         px(map, W / 2, H / 2), C_GREEN);
   printf("probe26: %s (%d checks, %d fail)\n", e.fails ? "FAIL" : "ok",
          e.checks, e.fails);
   return e.fails ? 1 : 0;
}
