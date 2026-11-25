/*
 * probe18 — pipeline из pNext execute важнее того, что осталось связанным.
 *
 * По спеке шейдерное состояние сгенерированных команд берётся не из связанного
 * pipeline'а: execution set разрешён только layout'у с токеном EXECUTION_SET
 * (VUID-VkGeneratedCommandsInfoEXT-indirectCommandsLayout-10241), а раз его
 * нет, в pNext обязан быть VkGeneratedCommandsPipelineInfoEXT
 * (VUID-...-indirectExecutionSet-11080). Именно так вызывает vkd3d, и именно
 * поэтому ему можно инвалидировать связанный pipeline перед тем, как он
 * сгенерировывает команды.
 *
 * Проверка связывает зелёный pipeline, а генерирует двумя вызовами: левый
 * треугольник — зелёным pipeline'ом (связанный и переданный совпадают), правый
 * — красным (связанный по-прежнему зелёный). Правая половина красным рисуется
 * только если драйвер прочитал pNext; иначе обе половины зелёные.
 *
 * RC: 0 = правая половина красная, 4 = нет, 3 = драйвер не отдал точки входа.
 */
#include "pcommon.h"
#include "vs_full_spv.h"
#include "fs_green_spv.h"
#include "fs_red_spv.h"

static PFN_vkCreateIndirectCommandsLayoutEXT pCreateLayout;
static PFN_vkGetGeneratedCommandsMemoryRequirementsEXT pMemReq;
static PFN_vkCmdExecuteGeneratedCommandsEXT pExecute;

static void
load_dgc(struct env *e)
{
   pCreateLayout = (PFN_vkCreateIndirectCommandsLayoutEXT)
      vkGetDeviceProcAddr(e->dev, "vkCreateIndirectCommandsLayoutEXT");
   pMemReq = (PFN_vkGetGeneratedCommandsMemoryRequirementsEXT)
      vkGetDeviceProcAddr(e->dev, "vkGetGeneratedCommandsMemoryRequirementsEXT");
   pExecute = (PFN_vkCmdExecuteGeneratedCommandsEXT)
      vkGetDeviceProcAddr(e->dev, "vkCmdExecuteGeneratedCommandsEXT");
}

/* Два треугольника: левый на вершинах 0..2, правый на 3..5. Порядок вершин
 * такой же ориентации, что и в probe13 (frontFace=CCW, cull=BACK). */
static const float g_tris[6][2] = {
   { -1.0f, -1.0f }, { -1.0f, 1.0f }, { 0.0f, 0.0f },  /* левый  */
   {  1.0f, -1.0f }, {  0.0f, 0.0f }, { 1.0f, 1.0f },  /* правый */
};

int
main(void)
{
   struct env e;
   struct env_ext ext = {.dgc = VK_TRUE};
   env_init_ex(&e, "probe18", &ext);
   load_dgc(&e);
   if (!pCreateLayout || !pMemReq || !pExecute) {
      fprintf(stderr, "probe18: DGC entry points missing "
                      "(is the extension advertised?)\n");
      return 3;
   }

   buf_create(&e, &e.vbo, &e.vmem, sizeof(g_tris),
              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 1);
   buf_write(&e, e.vmem, g_tris, sizeof(g_tris));

   /* Последовательность = VkDrawIndirectCommand; правый треугольник
    * начинается с вершины 3. */
   VkDrawIndirectCommand seqs[2] = {
      { .vertexCount = 3, .instanceCount = 1, .firstVertex = 0 },
      { .vertexCount = 3, .instanceCount = 1, .firstVertex = 3 },
   };
   VkBuffer seqbuf;
   VkDeviceMemory seqmem;
   /* indirectAddress требует INDIRECT_BUFFER (VUID-...-indirectAddress-12407). */
   VkDeviceAddress seq_va = bda_usage_buffer(&e, &seqbuf, &seqmem, sizeof(seqs),
                                            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
   buf_write(&e, seqmem, seqs, sizeof(seqs));

   struct stagex green[2] = {
      {VK_SHADER_STAGE_VERTEX_BIT, VS_FULL_SPV, VS_FULL_SPV_N * 4},
      {VK_SHADER_STAGE_FRAGMENT_BIT, FS_GREEN_SPV, FS_GREEN_SPV_N * 4},
   };
   struct stagex red[2] = {
      {VK_SHADER_STAGE_VERTEX_BIT, VS_FULL_SPV, VS_FULL_SPV_N * 4},
      {VK_SHADER_STAGE_FRAGMENT_BIT, FS_RED_SPV, FS_RED_SPV_N * 4},
   };
   VkPipelineLayout pl;
   VkPipeline p_green = make_pipeline(&e, green, 2, NULL, 0, &pl);
   VkPipeline p_red = make_pipeline(&e, red, 2, NULL, 0, NULL);

   VkIndirectCommandsLayoutTokenEXT tok = {
      .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,
      .type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT,
      .offset = 0,
   };
   VkIndirectCommandsLayoutCreateInfoEXT lci = {
      .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT,
      .shaderStages = VK_SHADER_STAGE_ALL_GRAPHICS,
      .indirectStride = sizeof(VkDrawIndirectCommand),
      .pipelineLayout = pl,
      .tokenCount = 1,
      .pTokens = &tok,
   };
   VkIndirectCommandsLayoutEXT lay;
   VKCHECK(pCreateLayout(e.dev, &lci, NULL, &lay));

   /* Один препоцесс-буфер на два вызова: layout один и тот же. */
   VkDeviceSize pp_size;
   VkBuffer ppbuf;
   VkDeviceMemory ppmem;
   VkDeviceAddress pp_va =
      preprocess_buffer(&e, pMemReq, lay, p_green, 1, &pp_size, &ppbuf, &ppmem);

   /* pipeline кладётся в pNext уже самого execute, поэтому у каждого вызова
    * свой VkGeneratedCommandsInfoEXT. */
   VkGeneratedCommandsPipelineInfoEXT with_green = {
      .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT,
      .pipeline = p_green,
   };
   VkGeneratedCommandsPipelineInfoEXT with_red = {
      .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT,
      .pipeline = p_red,
   };
   VkGeneratedCommandsInfoEXT gi[2] = {
      {
         .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT,
         .pNext = &with_green,
         .shaderStages = VK_SHADER_STAGE_ALL_GRAPHICS,
         .indirectCommandsLayout = lay,
         .indirectAddress = seq_va,
         .indirectAddressSize = sizeof(seqs[0]),
         .preprocessAddress = pp_va,
         .preprocessSize = pp_size,
         .maxSequenceCount = 1,
         .maxDrawCount = 1,
      },
      {
         .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT,
         .pNext = &with_red,
         .shaderStages = VK_SHADER_STAGE_ALL_GRAPHICS,
         .indirectCommandsLayout = lay,
         .indirectAddress = seq_va + sizeof(seqs[0]),
         .indirectAddressSize = sizeof(seqs[0]),
         .preprocessAddress = pp_va,
         .preprocessSize = pp_size,
         .maxSequenceCount = 1,
         .maxDrawCount = 1,
      },
   };

   VkCommandBufferBeginInfo cbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   VKCHECK(vkBeginCommandBuffer(e.cmd, &cbi));
   vkCmdBindPipeline(e.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p_green);
   VkDeviceSize off = 0;
   vkCmdBindVertexBuffers(e.cmd, 0, 1, &e.vbo, &off);
   begin_renderpass(&e, C_MAGENTA);
   pExecute(e.cmd, VK_FALSE, &gi[0]);
   pExecute(e.cmd, VK_FALSE, &gi[1]);
   end_renderpass(&e);
   VKCHECK(vkEndCommandBuffer(e.cmd));
   submit_wait(&e);

   void *map = readback(&e);
   const int y = H / 2;
   /* Центроиды треугольников по X; по Y оба накрывают середину экрана. */
   int cx_left = (int)((( (g_tris[0][0] + g_tris[1][0] + g_tris[2][0]) / 3.0f +
                          1.0f) * 0.5f) * W);
   int cx_right = (int)((( (g_tris[3][0] + g_tris[4][0] + g_tris[5][0]) / 3.0f +
                           1.0f) * 0.5f) * W);

   check(&e, "left bound", cx_left, y, px(map, cx_left, y), C_GREEN);
   check(&e, "right pNext", cx_right, y, px(map, cx_right, y), C_RED);
   /* Контроль сверху: оба треугольника в нижней половине сходятся к y=0, так
    * что у верхнего левого угла должен остаться алый clear. */
   check(&e, "no overdraw", 8, 6, px(map, 8, 6), C_MAGENTA);

   int ok = (e.fails == 0);
   printf("probe18: pipeline in pNext (%s) => %s\n",
          ok ? "overrides the bound pipeline" : "bound pipeline won",
          ok ? "execute uses the named pipeline" : "see FAIL lines");
   printf("probe18: %d checks, %d fail\n", e.checks, e.fails);
   return ok ? 0 : 4;
}
