/*==========================================================
 * Copyright 2020 QuickLogic Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *==========================================================*/

#include "string.h"
#include "Fw_global_config.h"

#if (FEATURE_D2HPROTOCOL_HOST == 1)
#include "FreeRTOS.h"
#include "semphr.h"
#include "RtosTask.h"
#include "eoss3_dev.h"
#include "h2d_protocol.h"
#include "qlspi_s3.h"
#include "dbg_uart.h"
#include "eoss3_hal_gpio.h"


typedef struct {
	H2D_Callback rx_cb_ptr;
} H2D_Callback_Info;

typedef struct H2D_Protocol_info {
    H2D_Platform_Info pfm_info;
    H2D_Callback_Info cb_info[MAX_NUM_CHANNEL];
    uint8_t init_done;    
}H2D_Protocol_info;


/*Lock will be acquired when tx api is called and 
released in ISR after device receives the data*/ 
SemaphoreHandle_t g_h2d_transmit_lock;          // SJ : may not be required on the host side ??

xTaskHandle xHandleTaskH2DRx;
QueueHandle_t H2DRx_MsgQ;


#define H2D_DATA_NUM_BYTES  (6)
#define H2D_SEQ_MASK        (0xF)
#define H2D_SEQ_BIT_POS     (4)
#define H2D_CHANNEL_MASK_0  (0xF)
#define H2D_CHANNEL_MASK_1  (0x3)
#define H2D_CHANNEL_MASK    (0x3F)
#define H2D_CHANNEL_BIT_POS (2)
#define H2D_CMD_MASK        (0x3F)

/* structure for msg to be sent to h2drx task*/
typedef struct {
    uint8_t msg;
}H2D_Rx_Pkt;

/*gloabal variable for h2d protocol info*/
H2D_Protocol_info g_h2d_protocol_info = {0};



/*
    buf[0]          buf[1]           buf[2]         buf[3]          buf[4]          buf[5]          buf[7]          buf[7]
*--------------------------------------------------------------------------------------------------------------------------------
*               |               |               |               |               |               |               |               |
*--------------------------------------------------------------------------------------------------------------------------------
*       |          |            |               |               |               |               |               |               |
*   seq     ch           cmd         data[0]          data[1]                                                       data[5]
   [7:4]   [3:0]         [5:0]
           +[7:6]
*/
/*global variable for tx packet*/
uint8_t g_h2d_tx_buf [H2D_PACKET_SIZE_IN_BYTES] = {0};

#pragma data_alignment = 32
uint8_t g_h2d_rx_buf [H2D_PACKET_SIZE_IN_BYTES] = {0};

/* Allocating 4kB for reading data received from the device.
 * This has enough room for receiving 128ms 16-bit audio data @16kHz */
#define H2D_MAX_DATA_SIZE   (4*1024)
uint8_t g_data_buf[H2D_MAX_DATA_SIZE] = {0};

//uint8_t g_data_buf_ready = 0;

/* Internal APIs*/

/*!
* \fn      H2D_packet create_tx_packet(H2D_Cmd_Info *h2d_cmd_info)
* \brief   Function to create h2d tx packet 
* \param   hh2d_cmd_info -- input as unpacked cmd packet
* \returns H2D_packet -- packed tx packet
*/
static void create_tx_packet(H2D_Cmd_Info *h2d_cmd_info){
  
    /* copy the 4 bits seq and 4 bits of channel number to byte 0*/
    g_h2d_tx_buf[0] = ((h2d_cmd_info->seq & H2D_SEQ_MASK) << H2D_SEQ_BIT_POS) |          \
                      ((h2d_cmd_info->channel & H2D_CHANNEL_MASK) >> H2D_CHANNEL_BIT_POS);
    
    /* copy the remaing 2 bits (lsb of channel num) of channel number and 6 bits cmd to byte 1*/
    g_h2d_tx_buf[1] = ((h2d_cmd_info->channel & H2D_CHANNEL_MASK) << (8-H2D_CHANNEL_BIT_POS)) |    \
                       (h2d_cmd_info->cmd & H2D_CMD_MASK);
    
    /*copy remaining data as it is*/
    memcpy( &(g_h2d_tx_buf[2]), &(h2d_cmd_info->data[0]),H2D_DATA_NUM_BYTES);
    
    return;
}

static void extract_rx_packet(H2D_Cmd_Info *h2d_cmd_info){
    
    h2d_cmd_info->seq = ((g_h2d_rx_buf[0] >> H2D_SEQ_BIT_POS) & H2D_SEQ_MASK);
    h2d_cmd_info->channel = ((g_h2d_rx_buf[0] & 0xF) << H2D_CHANNEL_BIT_POS) | \
                             (g_h2d_rx_buf[1] >> (8-H2D_CHANNEL_BIT_POS));
                             
    h2d_cmd_info->cmd = (g_h2d_rx_buf[1] & H2D_CMD_MASK );
    
#if (DEBUG_H2D_PROTOCOL == 1)    
//if(h2d_cmd_info->cmd != EVT_RAW_PKT_READY)
{
  printf("[H2D Protocol]: Rx pkt = 0x%02X, 0x%02X, 0x%02X, 0x%02X ",g_h2d_rx_buf[0],g_h2d_rx_buf[1],g_h2d_rx_buf[2],g_h2d_rx_buf[3]);
  printf("0x%02X, 0x%02X, 0x%02X, 0x%02X \n",g_h2d_rx_buf[4],g_h2d_rx_buf[5],g_h2d_rx_buf[6],g_h2d_rx_buf[7]);
}
//else {    printf("[H2D Protocol]: Rx RAW pkt = %d, %d \n",h2d_cmd_info->seq, h2d_cmd_info->cmd); }
#endif
    /*copy remaining data as it is*/
    memcpy(&(h2d_cmd_info->data[0]),&(g_h2d_rx_buf[2]),H2D_DATA_NUM_BYTES);
    return;
}

/*!
* \fn      void generate_interrupt_to_s3(void)
* \brief   function to generate interrupt to device (s3)
* \param   -
* \returns -
*/
 void generate_interrupt_to_device(void){

    uint8_t out_gpio = g_h2d_protocol_info.pfm_info.H2D_gpio;
    HAL_GPIO_Write(out_gpio, 1);
    #if H2D_LED_TEST == 1
    LedBlueOn();
    #endif
    
}

/*!
* \fn      void clear_interrupt_to_s3(void)
* \brief   function to clear interrupt to device (s3)
* \param   -
* \returns -
*/
 void clear_interrupt_to_device(void){
  
    uint8_t out_gpio = g_h2d_protocol_info.pfm_info.H2D_gpio;
    HAL_GPIO_Write(out_gpio, 0);
    #if H2D_LED_TEST == 1
    LedBlueOff();
    #endif
}

static inline uint8_t read_gpio_intr_val(uint8_t gpio_num)
{
    // This register will reflect the value of the IO regardless of the type/polarity 
    return ((INTR_CTRL->GPIO_INTR_RAW) & (1<< gpio_num) );
}

static inline uint8_t read_gpio_out_val(uint8_t gpio_num)
{
    // This register will reflect the value of the IO regardless of the type/polarity 
    return ((MISC_CTRL->IO_OUTPUT >> gpio_num ) & 1 );
}


/*!
* \fn      void generate_pulse_to_device(void)
* \brief   function to generate pulse to device (s3)
*          assumes that QL_INT is alread zero(low) when calling this api           
* \param   - 
* \returns -
*/
#if (USE_4PIN_D2H_PROTOCOL == 1)
static void generate_pulse_to_device(void){
uint8_t h2d_ack = g_h2d_protocol_info.pfm_info.H2D_ack;

    /* set the ack intr to device */
    HAL_GPIO_Write(h2d_ack, 1);

    int delay_in_ms = 1; // Add one ms delay
    vTaskDelay((delay_in_ms/portTICK_PERIOD_MS));

    /*clear ack intr to device */
    HAL_GPIO_Write(h2d_ack, 0);

    return;
}
#else
static void generate_pulse_to_device(void){
    
    /*generate intr to device (QL_INT high)*/
    generate_interrupt_to_device();
    
    int delay_in_ms = 1; // Add one ms delay
    vTaskDelay((delay_in_ms/portTICK_PERIOD_MS));
    
    /*clear intr to device (QL_INT low)*/
    clear_interrupt_to_device();

    return;
}
#endif
//Receiving a Data pkt is done in 2 stages. First the pkt info is read;
//Then the data is read using the pointer from the pkt info.
//In between there should not be any interruption.
//So, all the read_device_mem() are to be protected between start and end locks
static int start_recv_lock(void) {
   //take the lock
   if (xSemaphoreTake(g_h2d_transmit_lock, portMAX_DELAY) != pdTRUE) {
        dbg_fatal_error("Error unable to take lock to g_h2d_transmit_lock\n");
        return H2D_ERROR;
   }
   return H2D_STATUS_OK;
}
static int end_recv_lock(void) {
    //release the lock 
    if (xSemaphoreGive(g_h2d_transmit_lock) != pdTRUE) {
        dbg_fatal_error("Error : unable to release lock to g_h2d_transmit_lock\n");
    }
   return H2D_STATUS_OK;
}
/*!
* \fn      int read_device_mem(uint32_t addr, uint8_t * buf, uint32_t len)
* \brief   function to read from device memory using QLSPI
* \param   - memory address, destination buffer, length of data to be read
* \returns - status of qlspi read operation
*/
static inline int read_device_mem(uint32_t addr, uint8_t * buf, uint32_t len)
{
    int ret = 0;
    ret = QLSPI_Read_S3_Mem(addr, buf, len);
    return ret;
}

/*!
* \fn      void get_data_buf(uint8_t * dest_buf, uint32_t len_bytes)
* \brief   function to get data from the data_buf
* \param   -  destination buffer, length of data to be copied
* \returns - 
*/
void get_data_buf(uint8_t * dest_buf, uint32_t len_bytes)
{
    if(NULL == dest_buf){
        return;
    }
    memcpy((void *)dest_buf, &g_data_buf[0], len_bytes);
    return;
}
#if (USE_4PIN_D2H_PROTOCOL == 1)
/* Receive task handler */
void h2dRxTaskHandler(void *pParameter){
  
    BaseType_t qret;
    unsigned int h2dRxTaskStop = 0;
    H2D_Rx_Pkt h2drx_msg;

    while(!h2dRxTaskStop){
        //clear the Msg Q buffer 
        memset(&h2drx_msg, 0, sizeof(h2drx_msg));
        qret = xQueueReceive(H2DRx_MsgQ, &h2drx_msg, H2DRX_MSGQ_WAIT_TIME);
        configASSERT(qret == pdTRUE);
        uint8_t ack_gpio;

#if 0 //print interrupts states 
        //interrupts 
        uint8_t out_gpio,in_gpio, out_gpio_val, in_gpio_val;
        in_gpio = g_h2d_protocol_info.pfm_info.D2H_gpio;
        out_gpio = g_h2d_protocol_info.pfm_info.H2D_gpio;
        out_gpio_val = read_gpio_out_val(out_gpio);
        in_gpio_val = read_gpio_out_val(in_gpio);
        //acks
        uint8_t out_ack,in_ack, out_ack_val, in_ack_val;
        in_ack = g_h2d_protocol_info.pfm_info.D2H_ack;
        out_ack = g_h2d_protocol_info.pfm_info.H2D_ack;
        out_ack_val = read_gpio_out_val(out_ack);
        in_ack_val = read_gpio_out_val(in_ack);

        printf("[H2D %d %d %d %d]", out_gpio_val, in_gpio_val, out_ack_val, in_ack_val);
#endif

        switch(h2drx_msg.msg)     {
        case H2DRX_MSG_INTR_RCVD:
            /* This is an event from device.
                read the device mem for rx buf over qlspi
                extract the ch_num and invoke the callback
                check if second read required for this event and fill data_buf
            */
            //To prevent another process to use the SPI bus. Start the lock
            start_recv_lock();
            // read rx buf from device mem
            if (read_device_mem(H2D_READ_ADDR,(uint8_t *)&(g_h2d_rx_buf[0]), (H2D_PACKET_SIZE_IN_BYTES))) {   //SJ
                dbg_fatal_error("device memory read failed\n");
            }
            else {
                // extract info from rx buf and fill info pkt to be sent to callback
                H2D_Cmd_Info h2d_cmd;
                Rx_Cb_Ret cb_ret = {0};
                extract_rx_packet(&h2d_cmd);
                uint8_t  ch_num = h2d_cmd.channel;

#if (DEBUG_H2D_PROTOCOL == 1)    
                dbg_str_int("[H2D Protocol]: calling Rx callback channel = %d\n", ch_num);
#endif
                /*invoke the callback*/
                if (g_h2d_protocol_info.cb_info[ch_num].rx_cb_ptr) {
                    cb_ret = g_h2d_protocol_info.cb_info[ch_num].rx_cb_ptr(h2d_cmd,cb_ret.data_read_req);
                    //printf("[H2D Protocol]: finished calling\n");
                }
                
                /* keep checking from callback ret value if second read is needed */
                /* if yes, then read the data from addr and length passed by cb ret value*/
                while(1 == cb_ret.data_read_req){
                    //printf("cb_ret.data_read_req = %d\n", cb_ret.data_read_req);
                    // need to read data buffer from device memory
                	configASSERT( cb_ret.len <= H2D_MAX_DATA_SIZE );
                    if (read_device_mem(cb_ret.addr,(uint8_t *)&(g_data_buf[0]), cb_ret.len)) {
                        dbg_fatal_error("device memory read failed\n");
                    }
                    else{
#if (DEBUG_H2D_PROTOCOL == 1)
                        dbg_str("[H2D Protocol]: Read data for received event type.\n");
#endif
                        cb_ret = g_h2d_protocol_info.cb_info[ch_num].rx_cb_ptr(h2d_cmd,1);
                    }
                }
                generate_pulse_to_device();
#if (DEBUG_H2D_PROTOCOL == 1)
                dbg_str("[H2D Protocol]: Received event from device. Pulse sent\n");
#endif
            }
            //Release only after both pkt info and data are read 
            end_recv_lock();
            break;

        case H2DRX_MSG_ACK_RCVD:
            /* This is an ack from the device for host transmit
                Wait for intr from device to go low and then pull QL_INT low
            */
            ack_gpio = g_h2d_protocol_info.pfm_info.D2H_ack;
            // wait for ack intr to go low
            while (read_gpio_intr_val(ack_gpio)) {
                vTaskDelay(1); // Leave time for other process to execute
            }

            // pull QL_INT low
            clear_interrupt_to_device();
#if 0
            // release the lock
            if (xSemaphoreGive(g_h2d_transmit_lock) != pdTRUE) {
                dbg_fatal_error("Error : unable to release lock to g_h2d_transmit_lock\n");
            }
#endif
            break;

        default:
            dbg_str("Invalid msg event received\n");
            break;
        } //end of switch
    } //end of while(1)
  
    return;
}
#else //use 2-pin protocol
/* Receive task handler */
void h2dRxTaskHandler(void *pParameter){
  
    BaseType_t qret;
    unsigned int h2dRxTaskStop = 0;
    H2D_Rx_Pkt h2drx_msg;
    
    while(!h2dRxTaskStop){
        //clear the Msg Q buffer 
        memset(&h2drx_msg, 0, sizeof(h2drx_msg));
        qret = xQueueReceive(H2DRx_MsgQ, &h2drx_msg, H2DRX_MSGQ_WAIT_TIME);
        configASSERT(qret == pdTRUE);
        
        switch(h2drx_msg.msg)
        {
        case H2DRX_MSG_INTR_RCVD:
        {
            // check QL_INT level
            uint8_t out_gpio,in_gpio, out_gpio_val;
            in_gpio = g_h2d_protocol_info.pfm_info.D2H_gpio;
            out_gpio = g_h2d_protocol_info.pfm_info.H2D_gpio;

            out_gpio_val = read_gpio_out_val(out_gpio);
#if (DEBUG_H2D_PROTOCOL ==1)
            dbg_str("[H2D Protocol]: QL_INT value = %d\n", out_gpio_val);
#endif
#if 1 //print interrupts states 
      //printf("[H2D]: QL_INT = %d, S3_INT = %d\n", out_gpio_val, read_gpio_intr_val(in_gpio));
      printf("[H2D %d %d]", out_gpio_val, read_gpio_intr_val(in_gpio));
#endif
            
            if(out_gpio_val){          // if QL_INT is high
            
                /* This is an ack from the device for host transmit
                    Wait for intr from device to go low 
                    and then pull QL_INT low
                    */
            
                // wait for intr to go low
                while (read_gpio_intr_val(in_gpio)) {
                    vTaskDelay(1); // Leave 2 ticks for other process to execute
                }
              
                // pull QL_INT low
                clear_interrupt_to_device();
#if 0 //we give it after writting to S3. so should not release it
                // release the lock
                if (xSemaphoreGive(g_h2d_transmit_lock) != pdTRUE) {
                    dbg_fatal_error("[H2D Protocol] : Error : unable to release lock to g_h2d_transmit_lock\n");
                }
#endif                
#if (DEBUG_H2D_PROTOCOL ==1)
                dbg_str("[H2D Protocol]:Transmit from host complete. Received pulse from device\n");
#endif
            }
            else{           // H2D is low -- device is writing to us
                /* This is an event from device.
                    read the device mem for rx buf over qlspi
                    extract the ch_num and invoke the callback
                    check if second read required for this event and fill data_buf
                */
                //To prevent another process to use the SPI bus. Start the lock
                start_recv_lock();
                // read rx buf from device mem
                if (read_device_mem(H2D_READ_ADDR,(uint8_t *)&(g_h2d_rx_buf[0]), (H2D_PACKET_SIZE_IN_BYTES))) {   //SJ
                    dbg_str("device memory read failed\n");
                }
                else {
                    // extract info from rx buf and fill info pkt to be sent to callback
                    H2D_Cmd_Info h2d_cmd;
                    Rx_Cb_Ret cb_ret = {0};
                    extract_rx_packet(&h2d_cmd);
                    uint8_t  ch_num = h2d_cmd.channel;
                   
                    /*invoke the callback*/
                    if (g_h2d_protocol_info.cb_info[ch_num].rx_cb_ptr) {
                        cb_ret = g_h2d_protocol_info.cb_info[ch_num].rx_cb_ptr(h2d_cmd,cb_ret.data_read_req);
                    }
                    /* keep checking from callback ret value if second read is needed */
                    /* if yes, then read the data from addr and length passed by cb ret value*/
                    while(cb_ret.data_read_req == 1){
                        // need to read data buffer from device memory
                    	configASSERT( cb_ret.len <= H2D_MAX_DATA_SIZE );
                        if (read_device_mem(cb_ret.addr,(uint8_t *)&(g_data_buf[0]), cb_ret.len)) {
                            dbg_str("device memory read failed\n");
                        }
                        else{
#if DEBUG_H2D_PROTOCOL
                            dbg_str("[H2D_PROTOCOL] Read data for received event type.\n");
#endif
                            cb_ret = g_h2d_protocol_info.cb_info[ch_num].rx_cb_ptr(h2d_cmd,1);
                        }
                    }
                    generate_pulse_to_device();
#if DEBUG_H2D_PROTOCOL
                    dbg_str("[H2D_PROTOCOL] Received event from device. Pulse sent\n");
#endif
                }
                //Release only after both pkt info and data are read 
                end_recv_lock();
            }
          
            break;
        }
        }
    }
  
    return;
}
#endif //USE_4PIN_D2H_PROTOCOL

/*!
* \fn      void send_msg_to_h2drx_task_fromISR(uint8_t msg_type)
* \brief   send msg to  H2DRx_MsgQ
* \param   msg id to be sent
* \returns -
*/
void send_msg_to_h2drx_task_fromISR(uint8_t msg_type) {
    H2D_Rx_Pkt h2d_msg;
    h2d_msg.msg = msg_type;
    
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

	if( xQueueSendFromISR( H2DRx_MsgQ, &(h2d_msg), &xHigherPriorityTaskWoken ) != pdPASS ) {
        dbg_fatal_error("[H2D Protocol] : Error : unable to send msg to H2DRx_MsgQ from ISR\n");
    }		
	portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
    
    return;
}


/*!
* \fn      void service_intr_from_device(void)
* \brief   function to service interrupt received from the device (s3)
*          This function is called from ISR. should use only isr safe apis  
* \param   - 
* \returns -
*/
void service_intr_from_device(void){
    send_msg_to_h2drx_task_fromISR(H2DRX_MSG_INTR_RCVD);
    return;
}
/*!
* \fn      void service_ack_from_device(void)
* \brief   function to service ack interrupt received from the device (s3)
*          This function is called from ISR. should use only isr safe apis  
* \param   - 
* \returns -
*/
void service_ack_from_device(void *arg){
    send_msg_to_h2drx_task_fromISR(H2DRX_MSG_ACK_RCVD);
    return;
}


/*!
* \fn      signed portBASE_TYPE start_rtos_task_h2drx( void)
* \brief   Setup msg queue and Task Handler for H2D rx Task 
* \param   - 
* \returns - portBASE_TYPE pdPASS on success
*/
static signed portBASE_TYPE start_rtos_task_h2drx( void) {
    static uint8_t ucParameterToPass;
 
    /* Create queue for h2d rx Task */
    H2DRx_MsgQ = xQueueCreate( H2DRX_QUEUE_LENGTH, sizeof(H2D_Rx_Pkt) );
    vQueueAddToRegistry( H2DRx_MsgQ, "H2DRx_Q" );
    configASSERT( H2DRx_MsgQ );
    
    /* Create H2D Rx Task */
    xTaskCreate ( h2dRxTaskHandler, "H2DRxTaskHandler", STACK_SIZE_ALLOC(STACK_SIZE_TASK_H2D_RX),  &ucParameterToPass, PRIORITY_TASK_H2D_RX, &xHandleTaskH2DRx);
    configASSERT( xHandleTaskH2DRx );
    
    return pdPASS;
}
//make sure while generating the interrupt to device no task switching happens
//Check both input gpio (interrupt from device) and output gpio (ack to device)
//are low before generating the interrupt to the device
static int generate_protected_interrupt(void)
{
    uint8_t out_gpio,in_gpio;
    in_gpio = g_h2d_protocol_info.pfm_info.D2H_gpio;
    out_gpio = g_h2d_protocol_info.pfm_info.H2D_gpio;
    int return_value = 0;

    portENTER_CRITICAL();
    if( (read_gpio_intr_val(in_gpio) == 0) && 
        (read_gpio_out_val(out_gpio) == 0) )
        {
           // generate interrupt to device, QL_INT
           generate_interrupt_to_device();
           return_value = 1;
        }
    portEXIT_CRITICAL();

    return return_value;
}
/*!
* \fn      int h2d_transmit_cmd(H2D_Cmd_Info *h2d_cmd_info)
* \brief   api to transmit cmd to device 
* \param   hh2d_cmd_info -- input as unpacked cmd packet, addre where the cmd pckt is to be written
* \returns status of tx operation
*/
int h2d_transmit_cmd(H2D_Cmd_Info *h2d_cmd_info) {
    if( !g_h2d_protocol_info.init_done )
        return H2D_ERROR;
    
    // create tx packet
   create_tx_packet(h2d_cmd_info);

   if (xSemaphoreTake(g_h2d_transmit_lock, portMAX_DELAY) != pdTRUE) {
        dbg_fatal_error("[H2D Protocol] : Error unable to take lock to g_h2d_transmit_lock\n");
        return H2D_ERROR;
   }

#if 0   
    uint8_t in_gpio = g_h2d_protocol_info.pfm_info.D2H_gpio;
   //wait till intr from Device is low (AP_INT is low)
   //TIM!!! DOn;t see how this happens if Device is waiting for an ACK
   // wait for intr to go low
   while (read_gpio_intr_val(in_gpio)) {
        vTaskDelay(1); // Leave 2 ticks for other process to execute
        //ets_delay_us(10); //wait 10 micro secs
   }
#endif
   
   // transmit over qlspi
   if( QLSPI_Write_S3_Mem(H2D_WRITE_ADDR, (uint8_t *)&(g_h2d_tx_buf[0]), H2D_PACKET_SIZE_IN_BYTES )) {
        dbg_str_int("Error in h2d transmit ", __LINE__);
        //release the lock and return error
        if (xSemaphoreGive(g_h2d_transmit_lock) != pdTRUE) {
            dbg_fatal_error("[H2D Protocol] : Error : unable to release lock to g_h2d_transmit_lock\n");
        }
        return H2D_ERROR;
   }

    //must release the lock 
    if (xSemaphoreGive(g_h2d_transmit_lock) != pdTRUE) {
        dbg_fatal_error("[H2D Protocol] : Error : unable to release lock to g_h2d_transmit_lock\n");
    }

#if (USE_4PIN_D2H_PROTOCOL == 1)
   // generate interrupt to device independent of other states
   generate_interrupt_to_device();

#else //check the states before generating interrupt 

    //use protected read of both lines, and then generate interupt
    while(generate_protected_interrupt() == 0)
    {
        vTaskDelay(1);
    }
   
#endif //USE_4PIN_D2H_PROTOCOL
   return H2D_STATUS_OK;
}

/*!
* \fn      int h2d_register_rx_callback(H2D_Callback rx_cb, uint8_t ch_num)
* \brief   function to register rx callback for channel
* \param   callback function, channel number
* \returns - status of register operation
*/
int h2d_register_rx_callback(H2D_Callback rx_cb, uint8_t ch_num) {
    int ret = H2D_STATUS_OK;
    
    if ((NULL==rx_cb) || (ch_num >= MAX_NUM_CHANNEL)) {
        dbg_str("Invalid paramter for h2d register callback\n");
        ret = H2D_ERROR;
    }
    if(g_h2d_protocol_info.cb_info[ch_num].rx_cb_ptr != NULL) {
        dbg_str_int("callback for channel already registered. ch_num = \n", ch_num);
        ret = H2D_ERROR;
    } else {
        g_h2d_protocol_info.cb_info[ch_num].rx_cb_ptr = rx_cb;
    }
    
    return ret;
}

/*platform configuration*/
void h2d_platform_init (H2D_Platform_Info * pfm_info) {   
    // Nothing to do as of now
    return;
}

/*!
* \fn      int h2d_protocol_init(H2D_Platform_Info * h2d_platform_info)
* \brief   function to initialize the h2d communication,
*          creates the transmit lock and h2drx task
* \param   platform info (input and output interrupt gpio)
* \returns - status of init operation
*/
int h2d_protocol_init(H2D_Platform_Info * h2d_platform_info) {
    if( g_h2d_protocol_info.init_done ) {
        dbg_str("h2d protocol already intialized.\n");
        return H2D_STATUS_OK;
    }
    
    memcpy( &(g_h2d_protocol_info.pfm_info), h2d_platform_info, sizeof(H2D_Platform_Info)); 
    
    uint8_t out_gpio = g_h2d_protocol_info.pfm_info.H2D_gpio;
    HAL_GPIO_Write(out_gpio, 0);            // write 0 to the QL_INT at init

#if (USE_4PIN_D2H_PROTOCOL == 1)
    out_gpio = g_h2d_protocol_info.pfm_info.H2D_ack;
    HAL_GPIO_Write(out_gpio, 0);            // write 0 to the H2D_ack
#endif
    
    // Check to see if D2H is active, if so wait for it to go inactive
    if (read_gpio_intr_val(g_h2d_protocol_info.pfm_info.D2H_gpio)) {
        dbg_str("Waiting for D2H to go inactive\n");
        while (read_gpio_intr_val(g_h2d_protocol_info.pfm_info.D2H_gpio)) {
            //this delay should not be there before the task started
            //vTaskDelay(1);
            dbg_str("Waiting for D2H to go inactive\n");
        }
        dbg_str("D2H inactive - resuming config\n");
    }
    
    //create tx lock
    if(g_h2d_transmit_lock == NULL) {
        g_h2d_transmit_lock = xSemaphoreCreateBinary();
        if( g_h2d_transmit_lock == NULL ) {
          dbg_str("[H2D Protocol] : Error : Unable to Create Mutex\n");
          return H2D_ERROR;
        }
        vQueueAddToRegistry(g_h2d_transmit_lock, "h2d_transmit_lock" );
        xSemaphoreGive(g_h2d_transmit_lock);
    }
    
    
    // create the rx task
    start_rtos_task_h2drx();
    
    g_h2d_protocol_info.init_done = 1;
    
    return H2D_STATUS_OK;
}

#endif /* FEATURE_D2HPROTOCOL_HOST */