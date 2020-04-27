#include "speech_recognizer.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "sysctl.h"
#include "plic.h"
#include "uarths.h"
#include "i2s.h"
#include "Maix_i2s.h"
#include "fpioa.h"

#include "g_def.h"
#include "VAD.h"
#include "MFCC.h"
#include "DTW.h"
#include "flash.h"
#include "ADC.h"
extern v_ftr_tag ftr_save[20 * 4];
// #define USART1_printf Serial.printf

uint16_t VcBuf[atap_len];
atap_tag atap_arg;
valid_tag valid_voice[max_vc_con];
v_ftr_tag ftr;
v_ftr_tag ftr_temp;
v_ftr_tag ftr_mdl_temp[10];
v_ftr_tag *pftr_mdl_temp[10];

#define save_ok 0
#define VAD_fail 1
#define MFCC_fail 2
#define Flash_fail 3

#define FFT_N 512

uint16_t rx_buf[FRAME_LEN];
uint32_t g_rx_dma_buf[FRAME_LEN * 2];
uint64_t fft_out_data[FFT_N / 2];

volatile uint32_t g_index;
volatile uint8_t uart_rec_flag;
volatile uint32_t receive_char;
volatile uint8_t i2s_rec_flag;
volatile uint8_t i2s_start_flag = 0;

uint8_t comm; //关键词号
static const char *TAG = "speech_recognizer";

uint8_t speech_recognizer_save_mdl(uint16_t *v_dat, uint32_t addr);
uint8_t speech_recognizer_spch_recg(uint16_t *v_dat, uint32_t *mtch_dis);

int i2s_dma_irq(void *ctx)
{
    uint32_t i;
    if (i2s_start_flag)
    {
        int16_t s_tmp;
        if (g_index)
        {
            i2s_receive_data_dma(I2S_DEVICE_0, &g_rx_dma_buf[g_index], frame_mov * 2, DMAC_CHANNEL3);
            g_index = 0;
            for (i = 0; i < frame_mov; i++)
            {
                s_tmp = (int16_t)(g_rx_dma_buf[2 * i] & 0xffff); //g_rx_dma_buf[2 * i + 1] Low left
                rx_buf[i] = s_tmp + 32768;
            }
            i2s_rec_flag = 1;
        }
        else
        {
            i2s_receive_data_dma(I2S_DEVICE_0, &g_rx_dma_buf[0], frame_mov * 2, DMAC_CHANNEL3);
            g_index = frame_mov * 2;
            for (i = frame_mov; i < frame_mov * 2; i++)
            {
                s_tmp = (int16_t)(g_rx_dma_buf[2 * i] & 0xffff); //g_rx_dma_buf[2 * i + 1] Low left
                rx_buf[i] = s_tmp + 32768;
            }
            i2s_rec_flag = 2;
        }
    }
    else
    {
        i2s_receive_data_dma(I2S_DEVICE_0, &g_rx_dma_buf[0], frame_mov * 2, DMAC_CHANNEL3);
        g_index = frame_mov * 2;
    }
    return 0;
}

int speech_recognizer_init(i2s_device_number_t device_num)
{
    // io_mux_init
    fpioa_set_function(20, FUNC_I2S0_IN_D0);
    fpioa_set_function(18, FUNC_I2S0_SCLK);
    fpioa_set_function(19, FUNC_I2S0_WS);

    //i2s init
    i2s_init(device_num, I2S_RECEIVER, 0x3);

    i2s_rx_channel_config(device_num, I2S_CHANNEL_0,
                          RESOLUTION_16_BIT, SCLK_CYCLES_32,
                          TRIGGER_LEVEL_4, STANDARD_MODE);

    i2s_set_sample_rate(device_num, 8000);

    dmac_init();
    dmac_set_irq(DMAC_CHANNEL3, i2s_dma_irq, NULL, 3);
    i2s_receive_data_dma(device_num, &g_rx_dma_buf[0], frame_mov * 2, DMAC_CHANNEL3);

    /* Enable the machine interrupt */
    sysctl_enable_irq();
    return 0;
}
#include "syslog.h"
int speech_recognizer_record(uint8_t keyword_num, uint8_t model_num)
{
    if (keyword_num > 10)
        return -1;
    if (model_num > 4)
        return -2;

    comm = keyword_num;
    uint8_t prc_count = model_num;
    uint32_t addr = 0;

    g_index = 0;
    i2s_rec_flag = 0;
    i2s_start_flag = 1;

    addr = ftr_start_addr + comm * size_per_comm + prc_count * size_per_ftr;

    if (speech_recognizer_save_mdl(VcBuf, addr) == save_ok)
    {
        return 0;
    }
    else
    {
        return -3;
    }
}

int speech_recognizer_recognize(void)
{
    uint8_t res;
    uint32_t dis;

    g_index = 0;
    i2s_rec_flag = 0;
    i2s_start_flag = 1;

    res = speech_recognizer_spch_recg(VcBuf, &dis);
    if (dis != dis_err)
        return res;
    else
        return -1;
}

int speech_recognizer_add_voice_model(uint8_t keyword_num, uint8_t model_num, const int16_t *voice_model, uint16_t frame_num)
{
    ftr_save[keyword_num * 4 + model_num].save_sign = save_mask;
    ftr_save[keyword_num * 4 + model_num].frm_num = frame_num;

    for (int i = 0; i < (vv_frm_max * mfcc_num); i++)
        ftr_save[keyword_num * 4 + model_num].mfcc_dat[i] = voice_model[i];
    return 0;
}

int speech_recognizer_print_model(uint8_t keyword_num, uint8_t model_num)
{
    mp_printf(&mp_plat_print, "[MaixPy] frm_num=%d\n", ftr_save[keyword_num * 4 + model_num].frm_num);
    for (int i = 0; i < (vv_frm_max * mfcc_num); i++)
    {
        if (((i + 1) % 35) == 0)
            // mp_printf(&mp_plat_print, "[MaixPy] %d,\n", ftr_save[keyword_num * 4 + model_num].mfcc_dat[i]);
            printf("%d,\n", ftr_save[keyword_num * 4 + model_num].mfcc_dat[i]);
        else
            // mp_printf(&mp_plat_print, "[MaixPy] %d, ", ftr_save[keyword_num * 4 + model_num].mfcc_dat[i]);
            printf("%d, ", ftr_save[keyword_num * 4 + model_num].mfcc_dat[i]);
    }
    mp_printf(&mp_plat_print, "[MaixPy] \nprint model ok!\n");
    return 0;
}

uint8_t speech_recognizer_save_mdl(uint16_t *v_dat, uint32_t addr)
{
    uint16_t i, num;
    uint16_t frame_index;
get_noise1:
    frame_index = 0;
    num = atap_len / frame_mov;
    //wait for finish
    while (1)
    {
        while (i2s_rec_flag == 0)
            continue;
        if (i2s_rec_flag == 1)
        {
            for (i = 0; i < frame_mov; i++)
                v_dat[frame_mov * frame_index + i] = rx_buf[i];
        }
        else
        {
            for (i = 0; i < frame_mov; i++)
                v_dat[frame_mov * frame_index + i] = rx_buf[i + frame_mov];
        }
        i2s_rec_flag = 0;
        frame_index++;
        if (frame_index >= num)
            break;
    }

    noise_atap(v_dat, atap_len, &atap_arg);
    if (atap_arg.s_thl > 10000)
    {
        mp_printf(&mp_plat_print, "[MaixPy] get noise again...\n");
        goto get_noise1;
    }
    mp_printf(&mp_plat_print, "[MaixPy] speeking...\n");
    //wait for finish
    while (i2s_rec_flag == 0)
        continue;
    if (i2s_rec_flag == 1)
    {
        for (i = 0; i < frame_mov; i++)
            v_dat[i + frame_mov] = rx_buf[i];
    }
    else
    {
        for (i = 0; i < frame_mov; i++)
            v_dat[i + frame_mov] = rx_buf[i + frame_mov];
    }
    i2s_rec_flag = 0;
    while (1)
    {
        while (i2s_rec_flag == 0)
            continue;
        if (i2s_rec_flag == 1)
        {
            for (i = 0; i < frame_mov; i++)
            {
                v_dat[i] = v_dat[i + frame_mov];
                v_dat[i + frame_mov] = rx_buf[i];
            }
        }
        else
        {
            for (i = 0; i < frame_mov; i++)
            {
                v_dat[i] = v_dat[i + frame_mov];
                v_dat[i + frame_mov] = rx_buf[i + frame_mov];
            }
        }
        i2s_rec_flag = 0;
        if (VAD2(v_dat, valid_voice, &atap_arg) == 1)
            break;
        if (receive_char == 's')
            return MFCC_fail;
    }
    //  if (valid_voice[0].end == ((void *)0)) {
    //      mp_printf(&mp_plat_print, "[MaixPy] VAD_fail\n");
    //      return VAD_fail;
    //  }

    get_mfcc(&(valid_voice[0]), &ftr, &atap_arg);
    if (ftr.frm_num == 0)
    {
        //mp_printf(&mp_plat_print, "[MaixPy] MFCC_fail\n");
        return MFCC_fail;
    }
    //  ftr.word_num = valid_voice[0].word_num;
    return save_ftr_mdl(&ftr, addr);
    //  ftr_mdl_temp[addr] = ftr;
    //  return save_ok;
}

uint8_t speech_recognizer_spch_recg(uint16_t *v_dat, uint32_t *mtch_dis)
{
    uint16_t i;
    uint32_t ftr_addr;
    uint32_t min_dis;
    uint16_t min_comm;
    uint32_t cur_dis;
    v_ftr_tag *ftr_mdl;
    uint16_t num;
    uint16_t frame_index;
    uint32_t cycle0, cycle1;

get_noise2:
    frame_index = 0;
    num = atap_len / frame_mov;
    //wait for finish
    i2s_rec_flag = 0;
    while (1)
    {
        while (i2s_rec_flag == 0)
            continue;
        if (i2s_rec_flag == 1)
        {
            for (i = 0; i < frame_mov; i++)
                v_dat[frame_mov * frame_index + i] = rx_buf[i];
        }
        else
        {
            for (i = 0; i < frame_mov; i++)
                v_dat[frame_mov * frame_index + i] = rx_buf[i + frame_mov];
        }
        i2s_rec_flag = 0;
        frame_index++;
        if (frame_index >= num)
            break;
    }
    noise_atap(v_dat, atap_len, &atap_arg);
    if (atap_arg.s_thl > 10000)
    {
        mp_printf(&mp_plat_print, "[MaixPy] get noise again...\n");
        goto get_noise2;
    }

    mp_printf(&mp_plat_print, "[MaixPy] speeking...\n");

    //wait for finish
    while (i2s_rec_flag == 0)
        continue;
    if (i2s_rec_flag == 1)
    {
        for (i = 0; i < frame_mov; i++)
            v_dat[i + frame_mov] = rx_buf[i];
    }
    else
    {
        for (i = 0; i < frame_mov; i++)
            v_dat[i + frame_mov] = rx_buf[i + frame_mov];
    }
    i2s_rec_flag = 0;
    while (1)
    {
        while (i2s_rec_flag == 0)
            continue;
        if (i2s_rec_flag == 1)
        {
            for (i = 0; i < frame_mov; i++)
            {
                v_dat[i] = v_dat[i + frame_mov];
                v_dat[i + frame_mov] = rx_buf[i];
            }
        }
        else
        {
            for (i = 0; i < frame_mov; i++)
            {
                v_dat[i] = v_dat[i + frame_mov];
                v_dat[i + frame_mov] = rx_buf[i + frame_mov];
            }
        }
        i2s_rec_flag = 0;
        if (VAD2(v_dat, valid_voice, &atap_arg) == 1)
            break;
        if (receive_char == 's')
        {
            *mtch_dis = dis_err;
            mp_printf(&mp_plat_print, "[MaixPy] send 'c' to start\n");
            return 0;
        }
    }
    mp_printf(&mp_plat_print, "[MaixPy] vad ok\n");
    //  if (valid_voice[0].end == ((void *)0)) {
    //      *mtch_dis=dis_err;
    //      USART1_printf("VAD fail ");
    //      return (void *)0;
    //  }

    get_mfcc(&(valid_voice[0]), &ftr, &atap_arg);
    if (ftr.frm_num == 0)
    {
        *mtch_dis = dis_err;
        mp_printf(&mp_plat_print, "[MaixPy] MFCC fail ");
        return 0;
    }
    //  for (i = 0; i < ftr.frm_num * mfcc_num; i++) {
    //      if (i % 12 == 0)
    //          mp_printf(&mp_plat_print, "[MaixPy] \n");
    //      mp_printf(&mp_plat_print, "[MaixPy] %d ", ftr.mfcc_dat[i]);
    //  }
    //  ftr.word_num = valid_voice[0].word_num;
    mp_printf(&mp_plat_print, "[MaixPy] mfcc ok\n");
    i = 0;
    min_comm = 0;
    min_dis = dis_max;
    cycle0 = read_csr(mcycle);
    for (ftr_addr = ftr_start_addr; ftr_addr < ftr_end_addr; ftr_addr += size_per_ftr)
    {
        //  ftr_mdl=(v_ftr_tag*)ftr_addr;
        ftr_mdl = (v_ftr_tag *)(&ftr_save[ftr_addr / size_per_ftr]);
        cur_dis = ((ftr_mdl->save_sign) == save_mask) ? dtw(ftr_mdl, &ftr) : dis_err;
        if ((ftr_mdl->save_sign) == save_mask)
        {
            mp_printf(&mp_plat_print, "[MaixPy] no. %d, frm_num = %d, save_mask=%d", i + 1, ftr_mdl->frm_num, ftr_mdl->save_sign);
            mp_printf(&mp_plat_print, "[MaixPy] cur_dis=%d\n", cur_dis);
        }
        if (cur_dis < min_dis)
        {
            min_dis = cur_dis;
            min_comm = i + 1;
        }
        i++;
    }
    cycle1 = read_csr(mcycle) - cycle0;
    mp_printf(&mp_plat_print, "[MaixPy] [INFO] recg cycle = 0x%08x\n", cycle1);
    if (min_comm % 4)
        min_comm = min_comm / ftr_per_comm + 1;
    else
        min_comm = min_comm / ftr_per_comm;

    *mtch_dis = min_dis;
    return (int)min_comm; //(commstr[min_comm].intst_tr);
}