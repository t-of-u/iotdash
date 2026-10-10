#include "dma_blit.h"

#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <rom/ets_sys.h>

#include "prof.h"

static const char *TAG = "dma_blit";
static constexpr size_t BURST = 64;            // GDMA burst to PSRAM (S3 maximum)
static constexpr size_t MAX_ITEM_BYTES = 4032;  // per descriptor, multiple of BURST below 4095

volatile bool gBlitActive = false;  // a band DMA is writing PSRAM (read by the scan-out stats)

bool IRAM_ATTR DmaBlitter::onRxEof(gdma_channel_handle_t, gdma_event_data_t *, void *ctx) {
  gBlitActive = false;
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(static_cast<DmaBlitter *>(ctx)->done_, &woken);
  return woken == pdTRUE;
}

bool DmaBlitter::begin(int fbWidth, size_t bandBytes) {
  fbW_ = fbWidth;
  bandBytes_ = bandBytes;
  done_ = xSemaphoreCreateBinary();

  gdma_channel_alloc_config_t txCfg = {};
  txCfg.direction = GDMA_CHANNEL_DIRECTION_TX;
  txCfg.flags.reserve_sibling = 1;
  if (gdma_new_ahb_channel(&txCfg, &tx_) != ESP_OK) return false;
  gdma_channel_alloc_config_t rxCfg = {};
  rxCfg.direction = GDMA_CHANNEL_DIRECTION_RX;
  rxCfg.sibling_chan = tx_;
  if (gdma_new_ahb_channel(&rxCfg, &rx_) != ESP_OK) return false;

  // Memory-to-memory: both channels on the same free M2M trigger.
  gdma_trigger_t trig = GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_M2M, 0);
  uint32_t freeMask = 0;
  gdma_get_free_m2m_trig_id_mask(tx_, &freeMask);
  trig.instance_id = __builtin_ctz(freeMask);
  ESP_ERROR_CHECK(gdma_connect(rx_, trig));
  ESP_ERROR_CHECK(gdma_connect(tx_, trig));

  gdma_strategy_config_t strategy = {};
  strategy.owner_check = true;
  strategy.auto_update_desc = true;
  strategy.eof_till_data_popped = true;
  gdma_apply_strategy(tx_, &strategy);
  gdma_apply_strategy(rx_, &strategy);

  gdma_transfer_config_t transfer = {};
  transfer.max_data_burst_size = BURST;
  transfer.access_ext_mem = true;
  ESP_ERROR_CHECK(gdma_config_transfer(tx_, &transfer));
  ESP_ERROR_CHECK(gdma_config_transfer(rx_, &transfer));

  gdma_rx_event_callbacks_t cbs = {};
  cbs.on_recv_eof = onRxEof;
  ESP_ERROR_CHECK(gdma_register_rx_event_callbacks(rx_, &cbs, this));

  for (int i = 0; i < 2; i++) {
    gdma_link_list_config_t txList = {};
    txList.num_items = bandBytes_ / MAX_ITEM_BYTES + 2;
    txList.item_alignment = 4;
    if (gdma_new_link_list(&txList, &txList_[i]) != ESP_OK) return false;
    gdma_link_list_config_t rxList = {};
    rxList.num_items = MAX_ROWS;
    rxList.item_alignment = 4;
    if (gdma_new_link_list(&rxList, &rxList_[i]) != ESP_OK) return false;
    band_[i] = (uint16_t *)heap_caps_aligned_alloc(BURST, bandBytes_, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!band_[i]) {
      ESP_LOGE(TAG, "no internal RAM for a %u byte band", (unsigned)bandBytes_);
      return false;
    }
  }
  return true;
}

bool DmaBlitter::start(int slot, uint16_t *fb, const Rect &r, int y, int rows) {
  // The CPU never writes the frame buffers, so there is nothing to write back; whoever
  // reads them with the CPU (transitions, the scan-out fallback, screenshots) invalidates
  // its cached copy first.

  gdma_buffer_mount_config_t tx = {};
  tx.buffer = band_[slot];
  tx.buffer_alignment = 4;
  tx.length = (size_t)rows * r.w * 2;
  tx.flags.mark_eof = 1;
  tx.flags.mark_final = GDMA_FINAL_LINK_TO_NULL;
  if (gdma_link_mount_buffers(txList_[slot], 0, &tx, 1, nullptr) != ESP_OK) return false;

  gdma_buffer_mount_config_t rx[MAX_ROWS] = {};
  for (int i = 0; i < rows; i++) {
    rx[i].buffer = fb + (y + i) * fbW_ + r.x;
    rx[i].buffer_alignment = BURST;
    rx[i].length = r.w * 2;
  }
  rx[rows - 1].flags.mark_final = GDMA_FINAL_LINK_TO_NULL;
  if (gdma_link_mount_buffers(rxList_[slot], 0, rx, rows, nullptr) != ESP_OK) return false;

  xSemaphoreTake(done_, 0);
  gBlitActive = true;
  gdma_start(rx_, gdma_link_get_head_addr(rxList_[slot]));
  gdma_start(tx_, gdma_link_get_head_addr(txList_[slot]));
  return true;
}

bool DmaBlitter::wait() { return xSemaphoreTake(done_, pdMS_TO_TICKS(100)) == pdTRUE; }

bool DmaBlitter::render(uint16_t *fb, const Rect &r, const DrawFn &draw) {
  RT_PROF(PF_BLIT);
  if ((r.x % ALIGN_PX) || (r.w % ALIGN_PX) || r.w <= 0 || r.h <= 0) {
    ESP_LOGE(TAG, "rect x/w must be multiples of %d (got x=%d w=%d)", ALIGN_PX, r.x, r.w);
    return false;
  }
  int rowsPerBand = min((int)(bandBytes_ / (r.w * 2)), MAX_ROWS);
  int slot = 0;
  bool inFlight = false, ok = true;
  uint32_t t0 = micros();
  uint64_t sent = 0;
  for (int y = r.y; y < r.y + r.h; y += rowsPerBand) {
    int rows = min(rowsPerBand, r.y + r.h - y);
    draw(band_[slot], y, rows);  // this band's previous DMA finished before the other one started
    if (inFlight) {
      RT_PROF(PF_DMA_WAIT);
      ok &= wait();
    }
    if (maxRate_) {
      RT_PROF(PF_DMA_WAIT);  // don't start this band before the rate cap allows it
      uint32_t due = (uint32_t)(sent * 1000000ULL / maxRate_);
      int32_t ahead = (int32_t)(due - (micros() - t0));
      if (ahead > 0) esp_rom_delay_us(ahead);
    }
    size_t bytes = (size_t)rows * r.w * 2;
    if (scanOut_) {
      RT_PROF(PF_DMA_WAIT);
      scanOut_->waitBusWindow(bytes / 100 + 20);  // ~100 MB/s when the bus is shared with the CPUs
    }
    sent += bytes;
    ok &= start(slot, fb, r, y, rows);
    inFlight = true;
    slot ^= 1;
  }
  if (inFlight) ok &= wait();
  return ok;
}
