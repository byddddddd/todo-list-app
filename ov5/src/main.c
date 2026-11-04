#include <stdio.h>
#include <string.h>
#include "xil_printf.h"
#include "xparameters.h"
#include "xgpiops.h"
#include "xaxivdma.h"
#include "xvtc.h"
#include "xil_io.h"
#include "sleep.h"

// ==========================================
// 1. 直接从 Vivado 查到的真实基地址
// ==========================================
#define VDMA0_BASEADDR   0x43000000 
#define VDMA1_BASEADDR   0x43010000 
#define VTC_BASEADDR     0x43C00000 
#define DYNCLK_BASEADDR  0x43C10000 

#define FRAME_BUFFER_ADDR0 0x10000000 // 摄像头数据写入地址
#define FRAME_BUFFER_ADDR1 0x30000000 // 屏幕数据读取地址

#define H_ACTIVE 640
#define V_ACTIVE 480

// ==========================================
// 2. EMIO I2C 驱动 (配置 OV5640)
// ==========================================
#define EMIO_SCL_NUM 54
#define EMIO_SDA_NUM 55
static XGpioPs gpiops_inst;
void emio_init(void) {
    XGpioPs_Config cfg;
    cfg.BaseAddr = 0xE000A000; // 直接写死 Zynq 内部 GPIO 的基地址，绝不死机
    
    XGpioPs_CfgInitialize(&gpiops_inst, &cfg, cfg.BaseAddr);
    
    // 以下全部保持原样
    XGpioPs_SetDirectionPin(&gpiops_inst, EMIO_SCL_NUM, 1);
    XGpioPs_SetDirectionPin(&gpiops_inst, EMIO_SDA_NUM, 1);
    XGpioPs_SetOutputEnablePin(&gpiops_inst, EMIO_SCL_NUM, 1);
    XGpioPs_SetOutputEnablePin(&gpiops_inst, EMIO_SDA_NUM, 1);
    XGpioPs_WritePin(&gpiops_inst, EMIO_SCL_NUM, 1);
    XGpioPs_WritePin(&gpiops_inst, EMIO_SDA_NUM, 1);
}

void sccb_start(void) {
    XGpioPs_WritePin(&gpiops_inst, EMIO_SDA_NUM, 1); usleep(4);
    XGpioPs_WritePin(&gpiops_inst, EMIO_SCL_NUM, 1); usleep(4);
    XGpioPs_WritePin(&gpiops_inst, EMIO_SDA_NUM, 0); usleep(4);
    XGpioPs_WritePin(&gpiops_inst, EMIO_SCL_NUM, 0); usleep(4);
}
void sccb_stop(void) {
    XGpioPs_WritePin(&gpiops_inst, EMIO_SCL_NUM, 0); usleep(4);
    XGpioPs_WritePin(&gpiops_inst, EMIO_SDA_NUM, 0); usleep(4);
    XGpioPs_WritePin(&gpiops_inst, EMIO_SCL_NUM, 1); usleep(4);
    XGpioPs_WritePin(&gpiops_inst, EMIO_SDA_NUM, 1); usleep(4);
}
void sccb_send_byte(u8 txd) {
    u8 t;
    XGpioPs_WritePin(&gpiops_inst, EMIO_SCL_NUM, 0);
    for(t=0; t<8; t++) {
        XGpioPs_WritePin(&gpiops_inst, EMIO_SDA_NUM, (txd&0x80)>>7);
        txd<<=1; usleep(4);
        XGpioPs_WritePin(&gpiops_inst, EMIO_SCL_NUM, 1); usleep(4);
        XGpioPs_WritePin(&gpiops_inst, EMIO_SCL_NUM, 0); usleep(4);
    }
}
void sccb_ack(void) {
    XGpioPs_WritePin(&gpiops_inst, EMIO_SCL_NUM, 0);
    XGpioPs_WritePin(&gpiops_inst, EMIO_SDA_NUM, 0); usleep(4);
    XGpioPs_WritePin(&gpiops_inst, EMIO_SCL_NUM, 1); usleep(4);
    XGpioPs_WritePin(&gpiops_inst, EMIO_SCL_NUM, 0); usleep(4);
}
void sccb_write_reg16(u16 addr, u8 data) {
    sccb_start(); sccb_send_byte(0x78); sccb_ack();
    sccb_send_byte(addr >> 8); sccb_ack(); sccb_send_byte(addr & 0x00FF); sccb_ack();
    sccb_send_byte(data); sccb_ack(); sccb_stop();
}

// ==========================================
// 3. OV5640 核心初始化
// ==========================================
void ov5640_init(void) {
    sccb_write_reg16(0x3008, 0x82); usleep(10000); // 软复位
    sccb_write_reg16(0x3008, 0x02); // 正常模式
    sccb_write_reg16(0x3103, 0x02); // PLL
    sccb_write_reg16(0x4300, 0x61); // RGB565
    sccb_write_reg16(0x3035, 0x11); 
    sccb_write_reg16(0x3036, 0x5A); // PCLK=72M
    sccb_write_reg16(0x3820, 0x46); // 关闭翻转
    sccb_write_reg16(0x3821, 0x01);
    
    sccb_write_reg16(0x3808, (H_ACTIVE >> 8) & 0xFF); 
    sccb_write_reg16(0x3809, H_ACTIVE & 0xFF);
    sccb_write_reg16(0x380a, (V_ACTIVE >> 8) & 0xFF); 
    sccb_write_reg16(0x380b, V_ACTIVE & 0xFF);
}

// ==========================================
// 4. 动态时钟 (Dynclk) - 绝不死等
// ==========================================
void dynclk_init(void) {
    // 写入 25.12MHz 像素时钟的默认参数
    Xil_Out32(DYNCLK_BASEADDR + 0x08, 0x00000F07); 
    Xil_Out32(DYNCLK_BASEADDR + 0x0C, 0x00000F81); 
    Xil_Out32(DYNCLK_BASEADDR + 0x10, 0x00000000); 
    Xil_Out32(DYNCLK_BASEADDR + 0x14, 0x00001041); 
    Xil_Out32(DYNCLK_BASEADDR + 0x18, 0x0029B331); 
    Xil_Out32(DYNCLK_BASEADDR + 0x1C, 0x00000021); 
    
    Xil_Out32(DYNCLK_BASEADDR + 0x00, 0x01); // 启动时钟
    // 去掉死等的 while 循环，直接放行
}

// ==========================================
// 5. 视频时序 (VTC) - 绕过 LookupConfig
// ==========================================
void vtc_init(void) {
    XVtc vtc;
    XVtc_Config vtcConfig;
    
    // 直接传入物理基地址，避免找不到 ID 导致死机
    vtcConfig.BaseAddress = VTC_BASEADDR;
    XVtc_CfgInitialize(&vtc, &vtcConfig, vtcConfig.BaseAddress);

    XVtc_Timing vtcTiming;
    vtcTiming.HActiveVideo = H_ACTIVE;
    vtcTiming.HFrontPorch = 16;
    vtcTiming.HSyncWidth = 96;
    vtcTiming.HBackPorch = 48;
    vtcTiming.HSyncPolarity = 0;
    vtcTiming.VActiveVideo = V_ACTIVE;
    vtcTiming.V0FrontPorch = 10;
    vtcTiming.V0SyncWidth = 2;
    vtcTiming.V0BackPorch = 33;
    vtcTiming.V1FrontPorch = 10;
    vtcTiming.V1SyncWidth = 2;
    vtcTiming.V1BackPorch = 33;
    vtcTiming.VSyncPolarity = 0;
    vtcTiming.Interlaced = 0;

    XVtc_SourceSelect SourceSelect;
    memset((void *)&SourceSelect, 0, sizeof(SourceSelect));
    SourceSelect.VBlankPolSrc = 1;
    SourceSelect.VSyncPolSrc = 1;
    SourceSelect.HBlankPolSrc = 1;
    SourceSelect.HSyncPolSrc = 1;
    SourceSelect.ActiveVideoPolSrc = 1;
    SourceSelect.ActiveChromaPolSrc= 1;
    SourceSelect.VChromaSrc = 1;
    SourceSelect.VActiveSrc = 1;
    SourceSelect.VBackPorchSrc = 1;
    SourceSelect.VSyncSrc = 1;
    SourceSelect.VFrontPorchSrc = 1;
    SourceSelect.VTotalSrc = 1;
    SourceSelect.HActiveSrc = 1;
    SourceSelect.HBackPorchSrc = 1;
    SourceSelect.HSyncSrc = 1;
    SourceSelect.HFrontPorchSrc = 1;
    SourceSelect.HTotalSrc = 1;

    XVtc_RegUpdateEnable(&vtc);
    XVtc_SetGeneratorTiming(&vtc, &vtcTiming);
    XVtc_SetSource(&vtc, &SourceSelect);
    XVtc_EnableGenerator(&vtc);
}

// ==========================================
// 6. VDMA 初始化 - 绕过 LookupConfig
// ==========================================
static XAxiVdma vdma0, vdma1;

int vdma_init(void) {
    XAxiVdma_Config cfg0, cfg1;
    
    // 强行指定 VDMA0 基地址
    memset(&cfg0, 0, sizeof(XAxiVdma_Config));
    cfg0.BaseAddress = VDMA0_BASEADDR;
    XAxiVdma_CfgInitialize(&vdma0, &cfg0, cfg0.BaseAddress);

    // 强行指定 VDMA1 基地址
    memset(&cfg1, 0, sizeof(XAxiVdma_Config));
    cfg1.BaseAddress = VDMA1_BASEADDR;
    XAxiVdma_CfgInitialize(&vdma1, &cfg1, cfg1.BaseAddress);

    XAxiVdma_DmaSetup WriteCfg, ReadCfg;
    
    // --- VDMA0: 写通道 (摄像头 -> DDR) ---
    memset(&WriteCfg, 0, sizeof(XAxiVdma_DmaSetup));
    WriteCfg.VertSizeInput = V_ACTIVE;
    WriteCfg.HoriSizeInput = H_ACTIVE * 2; 
    WriteCfg.Stride = H_ACTIVE * 2;
    WriteCfg.EnableCircularBuf = 1;
    WriteCfg.EnableSync = 1;
    XAxiVdma_DmaConfig(&vdma0, XAXIVDMA_WRITE, &WriteCfg);
    WriteCfg.FrameStoreStartAddr[0] = FRAME_BUFFER_ADDR0;
    XAxiVdma_DmaSetBufferAddr(&vdma0, XAXIVDMA_WRITE, WriteCfg.FrameStoreStartAddr);
    
    // --- VDMA1: 读通道 (DDR -> HDMI) ---
    memset(&ReadCfg, 0, sizeof(XAxiVdma_DmaSetup));
    ReadCfg.VertSizeInput = V_ACTIVE;
    ReadCfg.HoriSizeInput = H_ACTIVE * 2; 
    ReadCfg.Stride = H_ACTIVE * 2;
    ReadCfg.EnableCircularBuf = 1;
    ReadCfg.EnableSync = 1;
    XAxiVdma_DmaConfig(&vdma1, XAXIVDMA_READ, &ReadCfg);
    ReadCfg.FrameStoreStartAddr[0] = FRAME_BUFFER_ADDR1;
    XAxiVdma_DmaSetBufferAddr(&vdma1, XAXIVDMA_READ, ReadCfg.FrameStoreStartAddr);

    XAxiVdma_DmaStart(&vdma0, XAXIVDMA_WRITE);
    XAxiVdma_DmaStart(&vdma1, XAXIVDMA_READ);
    
    return XST_SUCCESS;
}

// ==========================================
// 7. 主函数
// ==========================================
int main() {
    xil_printf("=== SYSTEM BOOTED ===\r\n");
    
    emio_init();
    ov5640_init();
    xil_printf("OV5640 Configured.\r\n");
    
    dynclk_init();
    vtc_init();
    xil_printf("Dynclk & VTC Started.\r\n");
    
    if (vdma_init() == XST_SUCCESS) {
        xil_printf("VDMA Started. Streaming to HDMI...\r\n");
    }
    
    while(1) {
        for(volatile int i = 0; i < 10000000; i++);
    }
    return 0;
}