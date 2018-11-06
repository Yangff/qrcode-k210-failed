#include <stdio.h>
#include <stdlib.h>
#include "sysctl.h"
#include "plic.h"
#include "dmac.h"
#include "lcd.h"
#include "dvp.h"
#include "ov2640.h"

#include "uarths.h"
#include "fpioa.h"
#include "gpiohs.h"
#include "st7789.h"
#include "sleep.h"
#include "zbar.h"
#define zbar_fourcc(a, b, c, d)                 \
        ((unsigned long)(a) |                   \
         ((unsigned long)(b) << 8) |            \
         ((unsigned long)(c) << 16) |           \
         ((unsigned long)(d) << 24))
#define OV2640

uint8_t buf_sel;
volatile uint8_t buf_used[2];
uint32_t *lcd_buf[2];

static int dvp_irq(void *ctx)
{
	if (dvp_get_interrupt(DVP_STS_FRAME_FINISH)) {
		dvp_clear_interrupt(DVP_STS_FRAME_START | DVP_STS_FRAME_FINISH);
		buf_used[buf_sel] = 1;
		buf_sel ^= 0x01;
		dvp_set_display_addr((uint32_t)lcd_buf[buf_sel]);
	} else {
		dvp_clear_interrupt(DVP_STS_FRAME_START);
		if (buf_used[buf_sel] == 0)
		{
			dvp_start_convert();
		}
	}
	return 0;
}

uint8_t grey_pic[320*240];

int main(void)
{
	uint64_t core_id = current_coreid();
	void *ptr;
	uint16_t manuf_id, device_id;

    if (core_id == 0)
    {
    	sysctl_pll_set_freq(SYSCTL_CLOCK_PLL0,400000000);
   	 	uarths_init();
   	 	printf("pll0 freq:%d\r\n",sysctl_clock_get_freq(SYSCTL_CLOCK_PLL0));
   	 	zbar_image_scanner_t *scanner = NULL;
   	 	//zbar_set_verbosity(100);
   	 	scanner = zbar_image_scanner_create();
   	 	zbar_image_scanner_set_config(scanner, 0, ZBAR_CFG_ENABLE, 1);
   	 	
		zbar_image_t *image = zbar_image_create();
   		zbar_image_set_format(image, zbar_fourcc('G','R','E','Y'));
	   	zbar_image_set_size(image, 320, 240);

		sysctl->power_sel.power_mode_sel6 = 1;
		sysctl->power_sel.power_mode_sel7 = 1;
		sysctl->misc.spi_dvp_data_enable = 1;

		plic_init();
		//需要重新配置DMA,LCD才能使用DMA刷屏
		dmac->reset = 0x01;
		while (dmac->reset)
			;
		dmac->cfg = 0x03;
		// LCD init
		printf("LCD init\r\n");
		lcd_init();
		lcd_clear(BLUE);
		// DVP init
		printf("DVP init\r\n");

		do {
			printf("init ov2640\r\n");
			ov2640_init();
			ov2640_read_id(&manuf_id, &device_id);
			printf("manuf_id:0x%04x,device_id:0x%04x\r\n", manuf_id, device_id);
		} while (manuf_id != 0x7FA2 || device_id != 0x2642);


		ptr = malloc(sizeof(uint8_t) * 320 * 240 * (2 * 2) + 127);
		if (ptr == NULL)
			return;

		lcd_buf[0] = (uint32_t *)(((uint32_t)ptr + 127) & 0xFFFFFF80);
		lcd_buf[1] = (uint32_t *)((uint32_t)lcd_buf[0] + 320 * 240 * 2);
		buf_used[0] = 0;
		buf_used[1] = 0;
		buf_sel = 0;
		uint8_t buf_inst = 0;
		dvp_config_interrupt(DVP_CFG_START_INT_ENABLE | DVP_CFG_FINISH_INT_ENABLE, 0);
		dvp_set_display_addr((uint32_t)lcd_buf[buf_sel]);
		plic_set_priority(IRQN_DVP_INTERRUPT, 1);
		plic_irq_register(IRQN_DVP_INTERRUPT, dvp_irq, NULL);
		plic_irq_enable(IRQN_DVP_INTERRUPT);
		// system start
		printf("system start\n");
		set_csr(mstatus, MSTATUS_MIE);
		dvp_clear_interrupt(DVP_STS_FRAME_START | DVP_STS_FRAME_FINISH);
		dvp_config_interrupt(DVP_CFG_START_INT_ENABLE | DVP_CFG_FINISH_INT_ENABLE, 1);

		while(1)
		{
				while(buf_used[buf_inst]==0);
				printf("framed\n\r");	
				uint8_t* buf_frame = lcd_buf[buf_inst];
				for (int i = 0; i < 320*240; i++) {
					#define RED(a)      (uint32_t)((((a) & 0xf800) >> 11) << 3)
					#define GREEN(a)    (uint32_t)((((a) & 0x07e0) >> 5) << 2)
					#define BLUE(a)     (uint32_t)(((a) & 0x001f) << 3)
					uint16_t pix = (((uint16_t)buf_frame[i * 2]) << 8) | buf_frame[i * 2 + 1];
					grey_pic[i] = (RED(pix) + GREEN(pix) + BLUE(pix)) / 3;
				}
				printf("transfer ok.\n\r");	
				zbar_image_set_data(image, grey_pic, 320 * 240, NULL);

				printf("set data ok.\n\r");	
				int n = zbar_scan_image(scanner, image);
				zbar_symbol_t *symbol = zbar_image_first_symbol(image);
			    for(; symbol; symbol = zbar_symbol_next(symbol))
			    {
			        zbar_symbol_type_t typ = zbar_symbol_get_type(symbol);
			        const char *data = zbar_symbol_get_data(symbol);
			        printf("decoded %s symbol \"%s\"\n",zbar_get_symbol_name(typ), data);
			        int pointCount=zbar_symbol_get_loc_size(symbol);
			        printf("point count: %d\n",pointCount);
			        int i;
			        for(i=0;i<pointCount;i++)
			        {
			            int x=zbar_symbol_get_loc_x(symbol,i);
			            int y=zbar_symbol_get_loc_y(symbol,i);
			            printf("point%d=(%d,%d)\n",i,x,y);
			        }
			    }

				lcd_draw_picture(0, 0, 320, 240, lcd_buf[buf_inst]);

				while (tft_busy());
				buf_used[buf_inst] = 0;
				buf_inst ^= 1;
		}
	}
	while(1);
}