/*
 * probe20 - explicit preprocess DGC smoke/perf test.
 * Layout uses EXPLICIT_PREPROCESS_BIT. We verify:
 *  - Preprocess + Execute(isPreprocessed=true) draws correctly
 *  - Execute(isPreprocessed=false) on same layout draws correctly
 *  - Timing comparison for pc_draw-like generic case is done separately in probe19.
 */
#include "pcommon.h"
#include "vs_full_spv.h"
#include "fs_green_spv.h"

static PFN_vkCreateIndirectCommandsLayoutEXT pCreateLayout;
static PFN_vkGetGeneratedCommandsMemoryRequirementsEXT pMemReq;
static PFN_vkCmdExecuteGeneratedCommandsEXT pExecute;
static PFN_vkCmdPreprocessGeneratedCommandsEXT pPreprocess;

struct seq_draw { uint32_t vertex_count, instance_count, first_vertex, first_instance; };

static uint64_t submit_wait_ns(struct env *e)
{
   uint64_t t0 = now_ns();
   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &e->cmd,
   };
   VKCHECK(vkQueueSubmit(e->queue, 1, &si, e->fence));
   VKCHECK(vkWaitForFences(e->dev, 1, &e->fence, VK_TRUE, UINT64_MAX));
   VKCHECK(vkResetFences(e->dev, 1, &e->fence));
   VKCHECK(vkResetCommandPool(e->dev, e->pool, 0));
   return now_ns() - t0;
}

static void run_case(struct env *e, VkIndirectCommandsLayoutEXT lay, VkPipeline pipe,
                     VkPipelineLayout pl, VkDeviceAddress seq_va, VkDeviceAddress pp_va,
                     VkDeviceSize pp_size, uint32_t n, VkBool32 preprocessed, const char *tag)
{
   (void)pl;
   VkGeneratedCommandsPipelineInfoEXT pinfo = { .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT, .pipeline = pipe };
   VkGeneratedCommandsInfoEXT gi = {
      .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT,
      .pNext = &pinfo,
      .shaderStages = VK_SHADER_STAGE_ALL_GRAPHICS,
      .indirectCommandsLayout = lay,
      .indirectAddress = seq_va,
      .indirectAddressSize = (VkDeviceSize)sizeof(struct seq_draw) * n,
      .preprocessAddress = pp_va,
      .preprocessSize = pp_size,
      .maxSequenceCount = n,
      .maxDrawCount = n,
   };

   if (preprocessed) {
      VkCommandBufferAllocateInfo cai = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=e->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
      VkCommandBuffer pcmd;
      VKCHECK(vkAllocateCommandBuffers(e->dev, &cai, &pcmd));
      VkCommandBufferBeginInfo pcbi = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      VKCHECK(vkBeginCommandBuffer(pcmd, &pcbi));
      pPreprocess(pcmd, &gi, pcmd);
      VkResult r = vkEndCommandBuffer(pcmd);
      if (r != VK_SUCCESS) {
         fprintf(stderr, "pre EndCommandBuffer=%d\n", (int)r);
         vkFreeCommandBuffers(e->dev, e->pool, 1, &pcmd);
         e->fails++;
         return;
      }
      VkSubmitInfo psi = {.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&pcmd};
      VKCHECK(vkQueueSubmit(e->queue, 1, &psi, e->fence));
      VKCHECK(vkWaitForFences(e->dev, 1, &e->fence, VK_TRUE, UINT64_MAX));
      VKCHECK(vkResetFences(e->dev, 1, &e->fence));
      vkFreeCommandBuffers(e->dev, e->pool, 1, &pcmd);
   }

   VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
   VKCHECK(vkBeginCommandBuffer(e->cmd, &cbi));
   vkCmdBindPipeline(e->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   begin_renderpass(e, C_MAGENTA);
   VkDeviceSize zero = 0;
   vkCmdBindVertexBuffers(e->cmd, 0, 1, &e->vbo, &zero);
   pExecute(e->cmd, preprocessed, &gi);
   end_renderpass(e);
   VKCHECK(vkEndCommandBuffer(e->cmd));
   submit_wait_ns(e);
   void *map = readback(e);
   check(e, tag, W/2, H/2, px(map, W/2, H/2), C_GREEN);
}

int main(void)
{
   struct env e;
   struct env_ext ext = {.dgc = VK_TRUE};
   env_init_ex(&e, "probe20", &ext);
   pCreateLayout = (PFN_vkCreateIndirectCommandsLayoutEXT)vkGetDeviceProcAddr(e.dev, "vkCreateIndirectCommandsLayoutEXT");
   pMemReq = (PFN_vkGetGeneratedCommandsMemoryRequirementsEXT)vkGetDeviceProcAddr(e.dev, "vkGetGeneratedCommandsMemoryRequirementsEXT");
   pExecute = (PFN_vkCmdExecuteGeneratedCommandsEXT)vkGetDeviceProcAddr(e.dev, "vkCmdExecuteGeneratedCommandsEXT");
   pPreprocess = (PFN_vkCmdPreprocessGeneratedCommandsEXT)vkGetDeviceProcAddr(e.dev, "vkCmdPreprocessGeneratedCommandsEXT");
   if (!pCreateLayout || !pMemReq || !pExecute || !pPreprocess) return 3;

   struct stagex st[2] = {{VK_SHADER_STAGE_VERTEX_BIT, VS_FULL_SPV, VS_FULL_SPV_N*4}, {VK_SHADER_STAGE_FRAGMENT_BIT, FS_GREEN_SPV, FS_GREEN_SPV_N*4}};
   VkPipelineLayout pl;
   VkPipeline pipe = make_pipeline_ex(&e, st, 2, NULL, 0, NULL, &pl);
   float verts[3][2] = {{-0.008f,0.008f},{0.008f,0.008f},{0.0f,-0.016f}};
   buf_create(&e, &e.vbo, &e.vmem, sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 1);
   buf_write(&e, e.vmem, verts, sizeof(verts));

   VkIndirectCommandsLayoutTokenEXT tok = {.sType=VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,.type=VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT,.offset=0};
   VkIndirectCommandsLayoutCreateInfoEXT lci = {
      .sType=VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT,
      .flags=VK_INDIRECT_COMMANDS_LAYOUT_USAGE_EXPLICIT_PREPROCESS_BIT_EXT,
      .shaderStages=VK_SHADER_STAGE_ALL_GRAPHICS,
      .indirectStride=sizeof(struct seq_draw),
      .pipelineLayout=pl, .tokenCount=1, .pTokens=&tok,
   };
   VkIndirectCommandsLayoutEXT lay;
   VKCHECK(pCreateLayout(e.dev, &lci, NULL, &lay));

   uint32_t N = 4;
   struct seq_draw seqs[4];
   for (int i=0;i<4;i++) seqs[i]=(struct seq_draw){3,1,0,0};
   VkBuffer sb; VkDeviceMemory sm;
   VkDeviceAddress seq_va = bda_usage_buffer(&e, &sb, &sm, sizeof(seqs), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
   buf_write(&e, sm, seqs, sizeof(seqs));

   VkBuffer ppb; VkDeviceMemory ppm; VkDeviceSize ppsz=0;
   VkDeviceAddress pp_va = preprocess_buffer(&e, pMemReq, lay, pipe, N, &ppsz, &ppb, &ppm);

   run_case(&e, lay, pipe, pl, seq_va, pp_va, ppsz, N, VK_FALSE, "explicit/unpre");
   run_case(&e, lay, pipe, pl, seq_va, pp_va, ppsz, N, VK_TRUE, "explicit/pre");
   printf("probe20: %s\n", e.fails?"FAIL":"ok");
   return e.fails?1:0;
}
