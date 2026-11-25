/*
 * probe17 — токен INDEX_BUFFER: индексный буфер и тип индекса на последовательность.
 *
 * Проверяются оба входных режима (Vulkan и DXGI, которым пользуется vkd3d) и
 * оба ходовых типа индексов. Последовательность описывает 32 байта:
 *
 *   [0..7]   адрес индексного буфера
 *   [8..11]  размер в байтах
 *   [12..15] формат: VkIndexType в режиме Vulkan, DXGI_FORMAT в режиме DXGI
 *   [16..31] VkDrawIndexedIndirectCommand
 *
 * На экране четыре колонки, в каждой свой треугольник, и рисуется он только
 * индексами из токена, поэтому прогон ловит три разные поломки:
 *
 *  1. токен проигнорирован и взято состояние bind: там нулевой индексный
 *     буфер, все индексы указывают в «дальнюю» вершину, колонки остаются
 *     алыми;
 *  2. потерян тип индекса: 16-битную последовательность, прочитанную как
 *     32-битную, разбивает на вырожденные треугольники; 32-битную,
 *     прочитанную как 16-битную, разбивает на [a, 0, b], а вершина 0 уведена
 *     в самый верх, так что треугольник (a, 0, b) центр колонки не накрывает;
 *  3. потерян адрес: вместо своего буфера читается чужой (нулевой), индексы
 *     снова уходят в дальнюю вершину.
 *
 * Каждый индексный буфер взят с запасом (16 записей при максимальном индексе
 * 12), потому что MAX_INDICES транслятора = размер/размер_индекса — граница,
 * за которой значения индексов обрезает.
 *
 * Executed вызывается легальным путём, тем же, которым идёт vkd3d: layout без
 * токена EXECUTION_SET обязан быть с indirectExecutionSet == VK_NULL_HANDLE
 * (VUID-VkGeneratedCommandsInfoEXT-indirectCommandsLayout-10241), а шейдерное
 * состояние тогда берётся из VkGeneratedCommandsPipelineInfoEXT в pNext
 * (VUID-...-indirectExecutionSet-11080, для требований к памяти — 11011/11012).
 * Pipeline перед execute биндится, как это делает vkd3d через
 * d3d12_command_list_update_graphics_pipeline().
 *
 * RC: 0 = все четыре колонки нарисованы, 4 = нет, 3 = драйвер не отдал точки
 * входа.
 */
#include "pcommon.h"
#include "vs_full_spv.h"
#include "fs_green_spv.h"

#define NCOL 4

/* Индекс 0 — «дальняя» вершина: любой треугольник с ней не накрывает центр
 * своей колонки (см. пункт 2 в шапке). */
#define IDX_BOGUS 0

/* Записей в индексном буфере: больше максимального индекса (12), чтобы
 * MAX_INDICES не резал значения индексов. */
#define IB_ENTRIES 16

static PFN_vkCreateIndirectCommandsLayoutEXT pCreateLayout;
static PFN_vkGetGeneratedCommandsMemoryRequirementsEXT pMemReq;
static PFN_vkCmdExecuteGeneratedCommandsEXT pExecute;

struct seq_ib {
   uint64_t index_addr;
   uint32_t index_size_B;
   uint32_t index_format; /* VkIndexType или DXGI_FORMAT */
   uint32_t index_count;
   uint32_t instance_count;
   uint32_t first_index;
   int32_t vertex_offset;
   uint32_t first_instance;
};

/* Форматы из D3D12_INDEX_BUFFER_VIEW, которые vkd3d кладёт в последовательность
 * как есть (DXGI_FORMAT_R32_UINT / R16_UINT). */
#define DXGI_R32_UINT 42
#define DXGI_R16_UINT 57

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

/* Треугольник колонки: две вершины вверху, одна внизу, так что центроид лежит
 * ровно на y=0 и проверка в середине колонки не зависит от ориентации y. */
static void
column_triangle(float cx, float v[3][2])
{
   v[0][0] = cx - 0.18f; v[0][1] = 0.6f;
   v[1][0] = cx + 0.18f; v[1][1] = 0.6f;
   v[2][0] = cx;         v[2][1] = -0.6f;
}

static float
col_center_x(int col)
{
   return -1.0f + (col + 0.5f) * (2.0f / NCOL);
}

static int
col_center_px(int col)
{
   return (int)(((col_center_x(col) + 1.0f) * 0.5f) * W);
}

/* Layout: токен индексов в нуле последовательности, аргументы indexed-рисования
 * на 16. Порядок важен — индексы должны идти перед рисованием. */
static VkIndirectCommandsLayoutEXT
make_layout(struct env *e, VkIndirectCommandsInputModeFlagBitsEXT mode,
            VkPipelineLayout pl)
{
   VkIndirectCommandsIndexBufferTokenEXT ibt = {.mode = mode};
   VkIndirectCommandsLayoutTokenEXT toks[2] = {
      {
         .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,
         .type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_INDEX_BUFFER_EXT,
         .offset = 0,
         .data.pIndexBuffer = &ibt,
      },
      {
         .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,
         .type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_EXT,
         .offset = 16,
      },
   };
   VkIndirectCommandsLayoutCreateInfoEXT lci = {
      .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT,
      .shaderStages = VK_SHADER_STAGE_ALL_GRAPHICS,
      .indirectStride = sizeof(struct seq_ib),
      .pipelineLayout = pl,
      .tokenCount = 2,
      .pTokens = toks,
   };
   VkIndirectCommandsLayoutEXT lay;
   VKCHECK(pCreateLayout(e->dev, &lci, NULL, &lay));
   return lay;
}

int
main(void)
{
   struct env e;
   struct env_ext ext = {.dgc = VK_TRUE};
   env_init_ex(&e, "probe17", &ext);
   load_dgc(&e);
   if (!pCreateLayout || !pMemReq || !pExecute) {
      fprintf(stderr, "probe17: DGC entry points missing "
                      "(is the extension advertised?)\n");
      return 3;
   }

   /* Вершина 0 — дальняя, дальше по треугольнику на колонку. */
   float verts[1 + NCOL * 3][2];
   verts[IDX_BOGUS][0] = -0.98f;
   verts[IDX_BOGUS][1] = 0.98f;
   for (int c = 0; c < NCOL; c++)
      column_triangle(col_center_x(c), &verts[1 + c * 3]);

   buf_create(&e, &e.vbo, &e.vmem, sizeof(verts),
              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 1);
   buf_write(&e, e.vmem, verts, sizeof(verts));

   /* По индексному буферу на колонку: у каждого свои значения, чтобы потеря
    * адреса дала чужой треугольник, а не такой же. */
   static const uint16_t cols16[NCOL][3] = { {1, 2, 3}, {4, 5, 6}, {7, 8, 9},
                                             {10, 11, 12} };
   static const uint32_t cols32[NCOL][3] = { {1, 2, 3}, {4, 5, 6}, {7, 8, 9},
                                             {10, 11, 12} };
   VkBuffer b16[NCOL], b32[NCOL];
   VkDeviceMemory m16[NCOL], m32[NCOL];
   VkDeviceAddress va16[NCOL], va32[NCOL];
   for (int c = 0; c < NCOL; c++) {
      uint16_t ib16[IB_ENTRIES] = {0};
      uint32_t ib32[IB_ENTRIES] = {0};
      for (int k = 0; k < 3; k++) {
         ib16[k] = cols16[c][k];
         ib32[k] = cols32[c][k];
      }
      va16[c] = bda_usage_buffer(&e, &b16[c], &m16[c], sizeof(ib16),
                                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
      va32[c] = bda_usage_buffer(&e, &b32[c], &m32[c], sizeof(ib32),
                                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
      buf_write(&e, m16[c], ib16, sizeof(ib16));
      buf_write(&e, m32[c], ib32, sizeof(ib32));
   }

   /* Мусорный индексный буфер на bind: если токен не применится, рисуется по
    * нему, то есть нулям, и все индексы укажут в дальнюю вершину. Нули тут
    * важны — свежая память нулём не гарантируется, а «алые колонки» должны
    * быть следствием именно игнорируемого токена. */
   VkBuffer bogus;
   VkDeviceMemory bogus_mem;
   uint32_t zero_ib[16] = {0};
   buf_create(&e, &bogus, &bogus_mem, sizeof(zero_ib),
              VK_BUFFER_USAGE_INDEX_BUFFER_BIT, 1);
   buf_write(&e, bogus_mem, zero_ib, sizeof(zero_ib));

   struct stagex st[2] = {
      {VK_SHADER_STAGE_VERTEX_BIT, VS_FULL_SPV, VS_FULL_SPV_N * 4},
      {VK_SHADER_STAGE_FRAGMENT_BIT, FS_GREEN_SPV, FS_GREEN_SPV_N * 4},
   };
   VkPipelineLayout pl;
   VkPipeline pipe = make_pipeline(&e, st, 2, NULL, 0, &pl);

   VkIndirectCommandsLayoutEXT lay_vk =
      make_layout(&e, VK_INDIRECT_COMMANDS_INPUT_MODE_VULKAN_INDEX_BUFFER_EXT,
                  pl);
   VkIndirectCommandsLayoutEXT lay_dx =
      make_layout(&e, VK_INDIRECT_COMMANDS_INPUT_MODE_DXGI_INDEX_BUFFER_EXT,
                  pl);

   /* Шейдерное состояние для execute берётся из pNext: ровно так же делает
    * vkd3d (generated_ext.pNext = &pipeline_info, indirectExecutionSet = 0). */
   VkGeneratedCommandsPipelineInfoEXT exec_pinfo = {
      .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT,
      .pipeline = pipe,
   };

   /* Две последовательности на режим: 16-битные индексы и 32-битные. */
   struct seq_ib seqs[NCOL];
   memset(seqs, 0, sizeof(seqs));
   const uint32_t vk_fmt[2] = { VK_INDEX_TYPE_UINT16, VK_INDEX_TYPE_UINT32 };
   const uint32_t dx_fmt[2] = { DXGI_R16_UINT, DXGI_R32_UINT };
   for (int c = 0; c < NCOL; c++) {
      const int t = c % 2; /* 0 = 16 бит, 1 = 32 бита */
      struct seq_ib *s = &seqs[c];
      s->index_addr = t ? va32[c] : va16[c];
      s->index_size_B = (t ? sizeof(uint32_t) : sizeof(uint16_t)) * IB_ENTRIES;
      s->index_format = (c < 2 ? vk_fmt : dx_fmt)[t];
      s->index_count = 3;
      s->instance_count = 1;
      /* firstIndex/vertexOffset/firstInstance нулевые: индексы уже указывают
       * на нужную колонку. */
   }

   VkBuffer seqbuf;
   VkDeviceMemory seqmem;
   /* indirectAddress требует VK_BUFFER_USAGE_2_INDIRECT_BUFFER_BIT_KHR
    * (VUID-VkGeneratedCommandsInfoEXT-indirectAddress-12407). */
   VkDeviceAddress seq_va = bda_usage_buffer(&e, &seqbuf, &seqmem, sizeof(seqs),
                                            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
   buf_write(&e, seqmem, seqs, sizeof(seqs));

   VkDeviceSize pp_size_vk, pp_size_dx;
   VkBuffer ppbuf_vk, ppbuf_dx;
   VkDeviceMemory ppmem_vk, ppmem_dx;
   VkDeviceAddress pp_vk =
      preprocess_buffer(&e, pMemReq, lay_vk, pipe, 2, &pp_size_vk, &ppbuf_vk, &ppmem_vk);
   VkDeviceAddress pp_dx =
      preprocess_buffer(&e, pMemReq, lay_dx, pipe, 2, &pp_size_dx, &ppbuf_dx, &ppmem_dx);

   VkGeneratedCommandsInfoEXT gi[2] = {
      {
         .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT,
         .pNext = &exec_pinfo,
         .shaderStages = VK_SHADER_STAGE_ALL_GRAPHICS,
         .indirectCommandsLayout = lay_vk,
         .indirectAddress = seq_va,
         .indirectAddressSize = 2 * sizeof(struct seq_ib),
         .preprocessAddress = pp_vk,
         .preprocessSize = pp_size_vk,
         .maxSequenceCount = 2,
         .maxDrawCount = 2,
      },
      {
         .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT,
         .pNext = &exec_pinfo,
         .shaderStages = VK_SHADER_STAGE_ALL_GRAPHICS,
         .indirectCommandsLayout = lay_dx,
         .indirectAddress = seq_va + 2 * sizeof(struct seq_ib),
         .indirectAddressSize = 2 * sizeof(struct seq_ib),
         .preprocessAddress = pp_dx,
         .preprocessSize = pp_size_dx,
         .maxSequenceCount = 2,
         .maxDrawCount = 2,
      },
   };

   VkCommandBufferBeginInfo cbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   VKCHECK(vkBeginCommandBuffer(e.cmd, &cbi));
   vkCmdBindPipeline(e.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   VkDeviceSize off = 0;
   vkCmdBindVertexBuffers(e.cmd, 0, 1, &e.vbo, &off);
   vkCmdBindIndexBuffer(e.cmd, bogus, 0, VK_INDEX_TYPE_UINT32);
   begin_renderpass(&e, C_MAGENTA);
   pExecute(e.cmd, VK_FALSE, &gi[0]);
   pExecute(e.cmd, VK_FALSE, &gi[1]);
   end_renderpass(&e);
   VKCHECK(vkEndCommandBuffer(e.cmd));
   submit_wait(&e);

   void *map = readback(&e);
   const int y = H / 2;
   static const char *name[NCOL] = {
      "vk uint16", "vk uint32", "dx uint16", "dx uint32",
   };
   for (int c = 0; c < NCOL; c++)
      check(&e, name[c], col_center_px(c), y, px(map, col_center_px(c), y),
            C_GREEN);
   /* И контроль сверху: треугольники занимают середину по высоте, так что у
    * самой верхней кромки должно быть видно алый clear. */
   check(&e, "no overdraw", col_center_px(0), 6, px(map, col_center_px(0), 6),
         C_MAGENTA);

   int ok = (e.fails == 0);
   printf("probe17: INDEX_BUFFER (%s) => %s\n",
          ok ? "both input modes, 16 and 32 bit" : "token not applied",
          ok ? "per-sequence index buffer works" : "see FAIL lines");
   printf("probe17: %d checks, %d fail\n", e.checks, e.fails);
   return ok ? 0 : 4;
}
