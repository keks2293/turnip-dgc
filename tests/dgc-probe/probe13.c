/*
 * probe13 — первый реальный вызов vkCmdExecuteGeneratedCommandsEXT.
 *
 * Один токен DRAW на последовательность, без push-констант: всё, что делает
 * транслятор (layout -> PM4-стрим на GPU -> барьер -> CP_INDIRECT_BUFFER),
 * проверяется на обычном fullscreen-треугольнике с полосами из probe12.
 * Если полосы есть, путь работает end-to-end; если нет — виноват транслятор,
 * а не машина (probe12 проверяет её отдельно).
 *
 * RC: 0 = полосы есть, 4 = нет, 3 = драйвер не отдал точки входа.
 */
#include "pcommon.h"
#include "vs_full_spv.h"
#include "fs_bands_spv.h"

static const float g_full[3][2] = {
    {-1.0f, -1.0f},
    {-1.0f,  3.0f},
    { 3.0f, -1.0f},
};

static PFN_vkCreateIndirectCommandsLayoutEXT pCreateLayout;
static PFN_vkCreateIndirectExecutionSetEXT pCreateIES;
static PFN_vkGetGeneratedCommandsMemoryRequirementsEXT pMemReq;
static PFN_vkCmdExecuteGeneratedCommandsEXT pExecute;

static void
load_dgc(struct env *e)
{
   pCreateLayout = (PFN_vkCreateIndirectCommandsLayoutEXT)
      vkGetDeviceProcAddr(e->dev, "vkCreateIndirectCommandsLayoutEXT");
   pCreateIES = (PFN_vkCreateIndirectExecutionSetEXT)
      vkGetDeviceProcAddr(e->dev, "vkCreateIndirectExecutionSetEXT");
   pMemReq = (PFN_vkGetGeneratedCommandsMemoryRequirementsEXT)
      vkGetDeviceProcAddr(e->dev, "vkGetGeneratedCommandsMemoryRequirementsEXT");
   pExecute = (PFN_vkCmdExecuteGeneratedCommandsEXT)
      vkGetDeviceProcAddr(e->dev, "vkCmdExecuteGeneratedCommandsEXT");
}

/* Device-address buffer lives in pcommon.h: buf_create() does not pass the
 * VkMemoryAllocateFlagsInfo that a device address needs. */
int
main(void)
{
   struct env e;
   struct env_ext ext = {.dgc = VK_TRUE};
   env_init_ex(&e, "probe13", &ext);
   load_dgc(&e);
   if (!pCreateLayout || !pCreateIES || !pMemReq || !pExecute) {
      fprintf(stderr, "probe13: DGC entry points missing "
                      "(is the extension advertised?)\n");
      return 3;
   }

   buf_create(&e, &e.vbo, &e.vmem, sizeof(g_full),
              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 1);
   buf_write(&e, e.vmem, g_full, sizeof(g_full));

   /* One sequence: VkDrawIndirectCommand{vertexCount, instanceCount,
    * firstVertex, firstInstance}. */
   VkBuffer seqbuf;
   VkDeviceMemory seqmem;
   VkDeviceAddress seq_va = bda_buffer(&e, &seqbuf, &seqmem, 16);
   VkDrawIndirectCommand seq = {.vertexCount = 3, .instanceCount = 1};
   buf_write(&e, seqmem, &seq, sizeof(seq));

   struct stagex st[2] = {
      {VK_SHADER_STAGE_VERTEX_BIT, VS_FULL_SPV, VS_FULL_SPV_N * 4},
      {VK_SHADER_STAGE_FRAGMENT_BIT, FS_BANDS_SPV, FS_BANDS_SPV_N * 4},
   };
   VkPipeline pipe = make_pipeline(&e, st, 2, NULL, 0, NULL);

   VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
   };
   VkPipelineLayout pl;
   VKCHECK(vkCreatePipelineLayout(e.dev, &plci, NULL, &pl));

   VkIndirectCommandsLayoutTokenEXT tok = {
      .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,
      .type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT,
      .offset = 0,
   };
   VkIndirectCommandsLayoutCreateInfoEXT lci = {
      .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT,
      .shaderStages = VK_SHADER_STAGE_ALL_GRAPHICS,
      .indirectStride = 16,
      .pipelineLayout = pl,
      .tokenCount = 1,
      .pTokens = &tok,
   };
   VkIndirectCommandsLayoutEXT lay;
   VKCHECK(pCreateLayout(e.dev, &lci, NULL, &lay));

   VkIndirectExecutionSetPipelineInfoEXT pinfo = {
      .sType = VK_STRUCTURE_TYPE_INDIRECT_EXECUTION_SET_PIPELINE_INFO_EXT,
      .initialPipeline = pipe,
      .maxPipelineCount = 1,
   };
   VkIndirectExecutionSetInfoEXT iinfo = {.pPipelineInfo = &pinfo};
   VkIndirectExecutionSetCreateInfoEXT ieci = {
      .sType = VK_STRUCTURE_TYPE_INDIRECT_EXECUTION_SET_CREATE_INFO_EXT,
      .type = VK_INDIRECT_EXECUTION_SET_INFO_TYPE_PIPELINES_EXT,
      .info = iinfo,
   };
   VkIndirectExecutionSetEXT ies;
   VKCHECK(pCreateIES(e.dev, &ieci, NULL, &ies));

   VkGeneratedCommandsMemoryRequirementsInfoEXT mri = {
      .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_MEMORY_REQUIREMENTS_INFO_EXT,
      .indirectExecutionSet = ies,
      .indirectCommandsLayout = lay,
      .maxSequenceCount = 1,
      .maxDrawCount = 1,
   };
   VkMemoryRequirements2 mr = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
   };
   pMemReq(e.dev, &mri, &mr);

   VkDeviceSize pp_size = mr.memoryRequirements.size ? mr.memoryRequirements.size
                                                     : 256;
   VkBuffer ppbuf;
   VkDeviceMemory ppmem;
   VkDeviceAddress pp_va = bda_buffer(&e, &ppbuf, &ppmem, pp_size);

   VkGeneratedCommandsInfoEXT gi = {
      .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT,
      .shaderStages = VK_SHADER_STAGE_ALL_GRAPHICS,
      .indirectExecutionSet = ies,
      .indirectCommandsLayout = lay,
      .indirectAddress = seq_va,
      .indirectAddressSize = sizeof(seq),
      .preprocessAddress = pp_va,
      .preprocessSize = pp_size,
      .maxSequenceCount = 1,
      .maxDrawCount = 1,
   };

   VkCommandBufferBeginInfo cbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   VKCHECK(vkBeginCommandBuffer(e.cmd, &cbi));
   vkCmdBindPipeline(e.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   VkDeviceSize off = 0;
   vkCmdBindVertexBuffers(e.cmd, 0, 1, &e.vbo, &off);
   begin_renderpass(&e, C_MAGENTA);
   pExecute(e.cmd, VK_FALSE, &gi);
   end_renderpass(&e);
   VKCHECK(vkEndCommandBuffer(e.cmd));
   submit_wait(&e);

   void *map = readback(&e);
   check(&e, "red band", 77, 128, px(map, 77, 128), C_RED);
   check(&e, "green band", 281, 128, px(map, 281, 128), C_GREEN);
   check(&e, "blue band", 435, 128, px(map, 435, 128), C_BLUE);

   int ok = (e.fails == 0);
   printf("probe13: ExecuteGeneratedCommandsEXT (DRAW token) => %s\n",
          ok ? "translator works end-to-end" : "no bands: translator path");
   printf("probe13: %d checks, %d fail\n", e.checks, e.fails);
   return ok ? 0 : 4;
}
