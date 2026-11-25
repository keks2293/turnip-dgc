/*
 * probe19 — тайминг: путь DGC против CPU-цикла, которым vkd3d исполняет
 * ExecuteIndirect без DGC. Это базовая линия: без неё «оптимизировать под
 * архитектуру» не с чем сравнивать.
 *
 * Один и тот же результат, два способа поставить команды:
 *
 *   dgc    : layout на N последовательностей + один vkCmdExecuteGeneratedCommandsEXT
 *   manual : N × (vkCmdPushConstants + vkCmdDraw[Indexed]) — ровно то, что
 *            делает vkd3d в d3d12_command_list_execute_indirect, когда DGC нет
 *
 * Варианты — формы, которые вообще умеет попросить vkd3d:
 *
 *   draw     : {DRAW}
 *   pc_draw  : {PUSH_CONSTANT 16B, DRAW}   — два токена подписи
 *              D3D12ExecuteIndirect (CONSTANT_BUFFER_VIEW + DRAW)
 *   indexed  : {INDEX_BUFFER(DXGI), DRAW_INDEXED} — ради этого реализован
 *              DXGI-режим входных данных
 *
 * Какие именно подписи у Starfield, здесь не проверяется (в журналах прогона
 * нет ни одной строки dgc: — dgcdump в игре не включался): меряются формы, а
 * не пересечение с игрой.
 *
 * Меряется медиана по 7–25 прогонам на точку (не среднее: одна страница,
 * вытесненная подкачкой, сдвигает среднее больше, чем сам эффект):
 *
 *   inner  — CPU-время самой постановки (execute / цикл дроу)
 *   rec    — CPU-время записи всего командного буфера
 *   total  — запись + submit + ожидание fence, то есть полный кадр
 *   Δ/seq  — предельная стоимость одной последовательности (N=1 против Nmax);
 *            именно она говорит, сколько DGC экономит на горячем пути
 *
 * Readback из end_renderpass() в замерных прогонах сознательно НЕ делается:
 * это фиксированное копирование 512×256, которое съедает кадр из одного дроу.
 * Проверка картинки остаётся в отдельных verify-прогонах.
 *
 * Геометрия одна и та же в обоих путях: PROBE19_FILL=tiny (треугольник ~2px,
 * GPU почти простаивает → видно CPU) или fat (треугольник на весь экран,
 * N-кратный овердрейд → видно упор в GPU, где DGC по определению ничего не
 * даёт).
 *
 * Первые числа и выводы по ним — docs/dgc-perf-baseline.md.
 *
 * RC: 0 = картинка совпала в обоих путях у всех трёх вариантов,
 *     3 = расширение объявлено, но entry point'ов нет. Штатно сюда не попасть
 *     (env_init_ex запрашивает расширение, и без него не создался бы device),
 *     это защита от кривого ICD.
 */
#include "pcommon.h"
#include "vs_full_spv.h"
#include "fs_green_spv.h"
#include "vs_pc_spv.h"

#define PC_BYTES 16
#define IB_ENTRIES 16

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

/* Как в probe17: индексный токен (адрес, размер, формат) и следом
 * VkDrawIndexedIndirectCommand. */
struct seq_idx {
   uint64_t index_addr;
   uint32_t index_size_B;
   uint32_t index_format;
   uint32_t index_count, instance_count, first_index, vertex_offset,
       first_instance;
};

struct variant {
   const char *name;
   enum kind kind;
   size_t stride;
   /* Каждый вариант со своим пайплайном: pc_draw требует шейдера, который
    * читает push-константы, остальным он не нужен. */
   VkPipeline pipe;
   VkPipelineLayout pl;
   VkIndirectCommandsLayoutEXT lay;
   VkDeviceAddress seq_va;
   VkDeviceAddress pp_va;
   VkDeviceSize pp_size;
   VkBuffer ib; /* только для manual-пути: DGC берёт индекс из последовательности */
};

struct meas {
   uint64_t inner[64], rec[64], total[64];
   int n;
};

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

static void
print_driver(struct env *e)
{
   VkPhysicalDeviceDriverProperties dp = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
   };
   VkPhysicalDeviceProperties2 pp = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = &dp,
   };
   vkGetPhysicalDeviceProperties2(e->phys, &pp);
   printf("probe19: device=%s driver=%s %s\n", pp.properties.deviceName,
          dp.driverName, dp.driverInfo);
}

static VkIndirectCommandsLayoutEXT
make_layout(struct env *e, enum kind kind, uint32_t stride, VkPipelineLayout pl)
{
   VkIndirectCommandsPushConstantTokenEXT pct = {
      .updateRange = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0,
                       .size = PC_BYTES },
   };
   VkIndirectCommandsIndexBufferTokenEXT ibt = {
      .mode = VK_INDIRECT_COMMANDS_INPUT_MODE_DXGI_INDEX_BUFFER_EXT,
   };
   VkIndirectCommandsLayoutTokenEXT toks[2];
   uint32_t n = 1;

   switch (kind) {
   case V_DRAW:
      toks[0] = (VkIndirectCommandsLayoutTokenEXT){
         .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,
         .type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT,
         .offset = 0,
      };
      break;
   case V_PC_DRAW:
      toks[0] = (VkIndirectCommandsLayoutTokenEXT){
         .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,
         .type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_CONSTANT_EXT,
         .offset = 0,
         .data.pPushConstant = &pct,
      };
      toks[1] = (VkIndirectCommandsLayoutTokenEXT){
         .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,
         .type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT,
         .offset = PC_BYTES,
      };
      n = 2;
      break;
   case V_INDEXED:
      toks[0] = (VkIndirectCommandsLayoutTokenEXT){
         .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,
         .type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_INDEX_BUFFER_EXT,
         .offset = 0,
         .data.pIndexBuffer = &ibt,
      };
      toks[1] = (VkIndirectCommandsLayoutTokenEXT){
         .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,
         .type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_EXT,
         .offset = 16,
      };
      n = 2;
      break;
   }

   VkIndirectCommandsLayoutCreateInfoEXT lci = {
      .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT,
      .shaderStages = VK_SHADER_STAGE_ALL_GRAPHICS,
      .indirectStride = stride,
      .pipelineLayout = pl,
      .tokenCount = n,
      .pTokens = toks,
   };
   VkIndirectCommandsLayoutEXT lay;
   VKCHECK(pCreateLayout(e->dev, &lci, NULL, &lay));
   return lay;
}

/* submit + ожидание + reset, с таймером. submit_wait() из pcommon.h для этого
 * не годится: он не возвращает время, а дублировать его тело здесь незачем. */
static uint64_t
submit_wait_ns(struct env *e)
{
   uint64_t t0 = now_ns();
   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &e->cmd,
   };
   VKCHECK(vkQueueSubmit(e->queue, 1, &si, e->fence));
   VKCHECK(vkWaitForFences(e->dev, 1, &e->fence, VK_TRUE, ~(uint64_t)0));
   VKCHECK(vkResetFences(e->dev, 1, &e->fence));
   VKCHECK(vkResetCommandBuffer(e->cmd, 0));
   return now_ns() - t0;
}

/* Замер одного пути. inner — стоимость самой постановки, rec — всей записи
 * командного буфера, total — записи вместе с submit и ожиданием. */
static struct meas
measure(struct env *e, const struct variant *v, uint32_t seq_count, int manual,
        int reps)
{
   struct meas m = {0};
   const VkCommandBufferBeginInfo cbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   VkGeneratedCommandsPipelineInfoEXT pinfo = {
      .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT,
      .pipeline = v->pipe,
   };
   uint32_t pc[PC_BYTES / 4] = {0x11223344, 5, 6, 7};

   for (int r = 0; r < reps; r++) {
      VKCHECK(vkBeginCommandBuffer(e->cmd, &cbi));
      /* DGC-путь берёт pipeline из pNext, manual — только из bind: без него
       * драйвер рисует без состояния. */
      vkCmdBindPipeline(e->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v->pipe);

      uint64_t t0 = now_ns();
      begin_renderpass(e, C_MAGENTA);

      /* Состояние вне таймера: оно одинаково в обоих путях, а в inner должна
       * остаться только постановка команд. */
      VkDeviceSize zero = 0;
      vkCmdBindVertexBuffers(e->cmd, 0, 1, &e->vbo, &zero);
      if (manual && v->kind == V_INDEXED)
         vkCmdBindIndexBuffer(e->cmd, v->ib, 0, VK_INDEX_TYPE_UINT16);

      uint64_t i0 = now_ns(), i1;
      if (manual) {
         for (uint32_t i = 0; i < seq_count; i++) {
            if (v->kind == V_PC_DRAW)
               vkCmdPushConstants(e->cmd, v->pl, VK_SHADER_STAGE_VERTEX_BIT, 0,
                                  PC_BYTES, pc);
            if (v->kind == V_INDEXED)
               vkCmdDrawIndexed(e->cmd, 3, 1, 0, 0, 0);
            else
               vkCmdDraw(e->cmd, 3, 1, 0, 0);
         }
      } else {
         VkGeneratedCommandsInfoEXT gi = {
            .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT,
            .pNext = &pinfo,
            .shaderStages = VK_SHADER_STAGE_ALL_GRAPHICS,
            .indirectCommandsLayout = v->lay,
            .indirectAddress = v->seq_va,
            .indirectAddressSize = (VkDeviceSize)v->stride * seq_count,
            .preprocessAddress = v->pp_va,
            .preprocessSize = v->pp_size,
            .maxSequenceCount = seq_count,
            .maxDrawCount = seq_count,
         };
         pExecute(e->cmd, VK_FALSE, &gi);
      }
      i1 = now_ns();

      /* Readback здесь не делаем — см. шапку. */
      vkCmdEndRenderPass(e->cmd);
      VKCHECK(vkEndCommandBuffer(e->cmd));
      uint64_t t1 = now_ns();
      uint64_t t2 = t1 + submit_wait_ns(e);

      m.inner[m.n] = i1 - i0;
      m.rec[m.n] = t1 - t0;
      m.total[m.n] = t2 - t0;
      m.n++;
   }
   return m;
}

/* Картинка: тот же прогон с readback, чтобы «быстрее» не оказалось «пусто». */
static void
verify(struct env *e, const struct variant *v, int manual, const char *what)
{
   const VkCommandBufferBeginInfo cbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   VkGeneratedCommandsPipelineInfoEXT pinfo = {
      .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT,
      .pipeline = v->pipe,
   };
   const uint32_t n = 4;
   /* non-zero: vs_pc уводит треугольник за экран при magic == 0 */
   uint32_t pc[PC_BYTES / 4] = {0x11223344, 1, 2, 3};

   VKCHECK(vkBeginCommandBuffer(e->cmd, &cbi));
   vkCmdBindPipeline(e->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v->pipe);
   begin_renderpass(e, C_MAGENTA);
   VkDeviceSize zero = 0;
   vkCmdBindVertexBuffers(e->cmd, 0, 1, &e->vbo, &zero);
   if (manual && v->kind == V_INDEXED)
      vkCmdBindIndexBuffer(e->cmd, v->ib, 0, VK_INDEX_TYPE_UINT16);
   if (manual) {
      for (uint32_t i = 0; i < n; i++) {
         if (v->kind == V_PC_DRAW)
            vkCmdPushConstants(e->cmd, v->pl, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               PC_BYTES, pc);
         if (v->kind == V_INDEXED)
            vkCmdDrawIndexed(e->cmd, 3, 1, 0, 0, 0);
         else
            vkCmdDraw(e->cmd, 3, 1, 0, 0);
      }
   } else {
      VkGeneratedCommandsInfoEXT gi = {
         .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT,
         .pNext = &pinfo,
         .shaderStages = VK_SHADER_STAGE_ALL_GRAPHICS,
         .indirectCommandsLayout = v->lay,
         .indirectAddress = v->seq_va,
         .indirectAddressSize = (VkDeviceSize)v->stride * n,
         .preprocessAddress = v->pp_va,
         .preprocessSize = v->pp_size,
         .maxSequenceCount = n,
         .maxDrawCount = n,
      };
      pExecute(e->cmd, VK_FALSE, &gi);
   }
   end_renderpass(e);
   VKCHECK(vkEndCommandBuffer(e->cmd));
   submit_wait(e);

   void *map = readback(e);
   check(e, what, W / 2, H / 2, px(map, W / 2, H / 2), C_GREEN);
}

int
main(void)
{
   struct env e;
   struct env_ext ext = {.dgc = VK_TRUE};
   env_init_ex(&e, "probe19", &ext);
   load_dgc(&e);
   if (!pCreateLayout || !pMemReq || !pExecute) {
      fprintf(stderr, "probe19: DGC entry points missing "
                      "(is the extension advertised?)\n");
      return 3;
   }
   print_driver(&e);

   const int fat = getenv("PROBE19_FILL") &&
                   !strcmp(getenv("PROBE19_FILL"), "fat");

   /* Оба пути рисуют один и тот же треугольник; ориентация та же, что в
    * probe17 (иначе cullMode=BACK съест все треугольники). */
   float verts[3][2];
   if (fat) {
      verts[0][0] = -1.0f; verts[0][1] = -1.0f;
      verts[1][0] = -1.0f; verts[1][1] = 3.0f;
      verts[2][0] = 3.0f;  verts[2][1] = -1.0f;
   } else {
      verts[0][0] = -0.008f; verts[0][1] = 0.008f;
      verts[1][0] = 0.008f;  verts[1][1] = 0.008f;
      verts[2][0] = 0.0f;    verts[2][1] = -0.016f;
   }

   /* Индекс для indexed-варианта: 3 значения с запасом по записям, чтобы
    * MAX_INDICES = размер/размер_индекса не резал сами индексы. */
   uint16_t ib[IB_ENTRIES] = {0, 1, 2};

   struct variant v[3] = {
      { "draw", V_DRAW, sizeof(struct seq_draw) },
      { "pc_draw", V_PC_DRAW, sizeof(struct seq_pc) },
      { "indexed", V_INDEXED, sizeof(struct seq_idx) },
   };

   /* Два пайплайна. Обычный — для draw и indexed. pc_draw нужен свой, с VS,
    * который читает push-константы: без обращения к ним ir3 не заводит общий
    * preamble, и токену PUSH_CONSTANT нечего патчить (драйвер на это и
    * ссылается, отказывая в execute). */
   struct stagex st_plain[2] = {
      {VK_SHADER_STAGE_VERTEX_BIT, VS_FULL_SPV, VS_FULL_SPV_N * 4},
      {VK_SHADER_STAGE_FRAGMENT_BIT, FS_GREEN_SPV, FS_GREEN_SPV_N * 4},
   };
   struct stagex st_pc[2] = {
      {VK_SHADER_STAGE_VERTEX_BIT, VS_PC_SPV, VS_PC_SPV_N * 4},
      {VK_SHADER_STAGE_FRAGMENT_BIT, FS_GREEN_SPV, FS_GREEN_SPV_N * 4},
   };
   VkPushConstantRange pcr = {
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = PC_BYTES,
   };
   VkPipelineLayout pl_plain, pl_pc;
   VkPipeline pipe_plain =
      make_pipeline_ex(&e, st_plain, 2, NULL, 0, NULL, &pl_plain);
   VkPipeline pipe_pc =
      make_pipeline_ex(&e, st_pc, 2, NULL, 0, &pcr, &pl_pc);

   const uint32_t NMAX = fat ? 128u : 512u;
   static const uint32_t NS_TINY[] = { 1, 8, 64, 512 };
   static const uint32_t NS_FAT[] = { 1, 16, 128 };
   const uint32_t *NS = fat ? NS_FAT : NS_TINY;
   const int NNS = fat ? (int)(sizeof(NS_FAT) / sizeof(NS_FAT[0]))
                       : (int)(sizeof(NS_TINY) / sizeof(NS_TINY[0]));

   buf_create(&e, &e.vbo, &e.vmem, sizeof(verts),
              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 1);
   buf_write(&e, e.vmem, verts, sizeof(verts));

   VkBuffer ibo;
   VkDeviceMemory ibom;
   bda_usage_buffer(&e, &ibo, &ibom, sizeof(ib),
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
   buf_write(&e, ibom, ib, sizeof(ib));
   VkBufferDeviceAddressInfo ib_ai = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = ibo,
   };
   const VkDeviceAddress ib_va = vkGetBufferDeviceAddress(e.dev, &ib_ai);

   /* Данные последовательностей: одинаковые команды, кроме pc_draw, где
    * константы различаются (иначе драйвер мог бы «сэкономить» нечестно). */
   for (int i = 0; i < 3; i++) {
      VkBuffer sb, ppbuf;
      VkDeviceMemory sm, ppmem;
      const VkDeviceSize sz = (VkDeviceSize)v[i].stride * NMAX;
      uint8_t *seqs = calloc(NMAX, v[i].stride);
      for (uint32_t k = 0; k < NMAX; k++) {
         uint8_t *p = seqs + (size_t)k * v[i].stride;
         switch (v[i].kind) {
         case V_DRAW: {
            struct seq_draw d = {3, 1, 0, 0};
            memcpy(p, &d, sizeof(d));
            break;
         }
         case V_PC_DRAW: {
            struct seq_pc s = { .pc = { 0x1000 + k, 1, 2, 3 },
                                .draw = { 3, 1, 0, 0 } };
            memcpy(p, &s, sizeof(s));
            break;
         }
         case V_INDEXED: {
            struct seq_idx s = { .index_addr = ib_va,
                                 .index_size_B = sizeof(ib),
                                 .index_format = 57 /* DXGI_R16_UINT */,
                                 .index_count = 3,
                                 .instance_count = 1 };
            memcpy(p, &s, sizeof(s));
            break;
         }
         }
      }
      /* indirectAddress требует INDIRECT_BUFFER (VUID-...-indirectAddress-12407). */
      v[i].seq_va = bda_usage_buffer(&e, &sb, &sm, sz,
                                     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      buf_write(&e, sm, seqs, sz);
      free(seqs);
      seqs = NULL;

      v[i].pipe = (v[i].kind == V_PC_DRAW) ? pipe_pc : pipe_plain;
      v[i].pl = (v[i].kind == V_PC_DRAW) ? pl_pc : pl_plain;
      v[i].lay = make_layout(&e, v[i].kind, (uint32_t)v[i].stride, v[i].pl);
      /* preprocess_buffer() создаёт буфер сам и пишет его в buf/mem, поэтому
       * переменные отдельные: handles буфера последовательностей иначе
       * перезаписались бы. */
      v[i].pp_va = preprocess_buffer(&e, pMemReq, v[i].lay, v[i].pipe, NMAX,
                                     &v[i].pp_size, &ppbuf, &ppmem);
      v[i].ib = ibo;
   }

   /* Индексный буфер для manual-пути биндится в самом замере; здесь только
    * проверим, что оба пути рисуют. */
   printf("probe19: fill=%s N=", fat ? "fat" : "tiny");
   for (int i = 0; i < NNS; i++)
      printf("%u%s", NS[i], i + 1 < NNS ? "," : "");
   printf("\n");

   for (int i = 0; i < 3; i++) {
      char tag[64];
      snprintf(tag, sizeof(tag), "%s/dgc", v[i].name);
      verify(&e, &v[i], 0, tag);
      snprintf(tag, sizeof(tag), "%s/manual", v[i].name);
      verify(&e, &v[i], 1, tag);
   }

   printf("\n%-9s %5s | %10s %10s %10s | %10s %10s %10s | %7s %7s\n", "variant",
          "N", "dgc inner", "dgc rec", "dgc total", "man inner", "man rec",
          "man total", "inner/x", "total/x");
   for (int i = 0; i < 3; i++) {
      struct meas d_first = {0}, m_first = {0};
      int have_first = 0;
      for (int j = 0; j < NNS; j++) {
         const uint32_t n = NS[j];
         int reps = fat ? (n <= 1 ? 9 : 5) : (n <= 8 ? 25 : (n <= 64 ? 15 : 7));
         struct meas d = measure(&e, &v[i], n, 0, reps);
         struct meas mn = measure(&e, &v[i], n, 1, reps);
         const uint64_t d_in = median_u64(d.inner, d.n);
         const uint64_t d_rc = median_u64(d.rec, d.n);
         const uint64_t d_to = median_u64(d.total, d.n);
         const uint64_t m_in = median_u64(mn.inner, mn.n);
         const uint64_t m_rc = median_u64(mn.rec, mn.n);
         const uint64_t m_to = median_u64(mn.total, mn.n);
         printf("%-9s %5u | %10.2f %10.2f %10.2f | %10.2f %10.2f %10.2f | %7.2f %7.2f\n",
                v[i].name, n, us(d_in), us(d_rc), us(d_to), us(m_in), us(m_rc),
                us(m_to), d_in ? (double)m_in / (double)d_in : 0.0,
                d_to ? (double)m_to / (double)d_to : 0.0);
         if (!have_first) {
            d_first = d;
            m_first = mn;
            have_first = 1;
         }
         if (j == NNS - 1 && have_first && n > 1) {
            const uint32_t dn = n - NS[0];
            const uint64_t d_in = median_u64(d.inner, d.n);
            const uint64_t m_in = median_u64(mn.inner, mn.n);
            const uint64_t f_d = median_u64(d_first.inner, d_first.n);
            const uint64_t f_m = median_u64(m_first.inner, m_first.n);
            printf("%-9s %5s   предельная стоимость последовательности: "
                   "dgc %.3f us, manual %.3f us\n",
                   v[i].name, "Δ/seq",
                   (double)(d_in > f_d ? d_in - f_d : 0) / 1000.0 / dn,
                   (double)(m_in > f_m ? m_in - f_m : 0) / 1000.0 / dn);
         }
      }
   }

   printf("\nprobe19: %d checks, %d fail\n", e.checks, e.fails);
   return e.fails ? 4 : 0;
}
