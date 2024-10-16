/*******************************************************************************
 * Copyright (C) 2024 by Vishnu Kumar Thoodur Venkatachalapathy
 *
 * Redistribution, modification or use of this software in source or binary
 * forms is permitted as long as the files maintain this copyright. Users are
 * permitted to modify this and use it to learn about the field of embedded
 * software. Vishnu Kumar Thoodur Venkatachalapathy and the University of
 * Colorado are not liable for any misuse of this material.
 * ****************************************************************************/

/**
 * @file    scheduler.c
 * @brief   Implementation of scheduler
 *
 * @author  Vishnu Kumar Thoodur Venkatachalapathy
 * @date    Feb 7, 2024
 */

#include <em_core.h>
#include <sl_power_manager.h>
#include "src/scheduler.h"
#include "src/gpio.h"
#include "src/timers.h"
#include "i2c.h"
#include "lcd.h"
#include "ble.h"
#include "ble_device_type.h"

// Include logging specifically for this .c file
#define INCLUDE_LOG_DEBUG 1
#include "src/log.h"

#define LETIMERUF_BIT_POS 0
#define LETIMERCOMP1_BIT_POS 1
#define I2C_TRANSFER_COMPLETE_BIT_POS 2

#define SI7021_POR_TIME_US 80000
#define SI7021_14B_CONVERSION_TIME_US 10800

#define I2CTransferDone  0    /* Transfer completed successfully. Taken from em_i2c library*/
uint32_t SchedulerEvents = 0;

#define NUM_STATES 5

// enum declarations used for temperature state machines
typedef enum uint32_t {
  stateIdle,
  waitForSi7021POR,
  waitForI2CWriteTransfer,
  waitForSi7021Conversion,
  waitForI2CReadTransfer
} State_t;

typedef enum uint16_t{
  State_1,
  State_2,
  State_3,
  State_4,
  State_5,
  State_6,
  State_7
} Discovery_States_t;

uint8_t htm_temperature_buffer[5];
uint32_t htm_temperature_flt;
uint8_t flags = 0x00;

#if BUILD_INCLUDES_BLE_CLIENT == 1

// Health Thermometer service UUID defined by Bluetooth SIG
static const uint8_t thermo_service[2] = { 0x09, 0x18 };
// Temperature Measurement characteristic UUID defined by Bluetooth SIG
static const uint8_t thermo_char[2] = { 0x1c, 0x2a };


// Custom Button service UUID:
// 00000001-38c8-433e-87ec-652a2d136289
static const uint8_t button_service[] = { 0x89, 0x62, 0x13, 0x2d, 0x2a, 0x65, // 652a2d136289
                                          0xec, 0x87,                         // 87ec
                                          0x3e, 0x43,                         // 433e
                                          0xc8, 0x38,                         // 38c8
                                          0x01, 0x00, 0x00, 0x00              // 00000001
                                        };
// Custom Button characteristic UUID:
// 00000002-38c8-433e-87ec-652a2d136289
static const uint8_t button_char[] =  { 0x89, 0x62, 0x13, 0x2d, 0x2a, 0x65, // 652a2d136289
                                        0xec, 0x87,                         // 87ec
                                        0x3e, 0x43,                         // 433e
                                        0xc8, 0x38,                         // 38c8
                                        0x02, 0x00, 0x00, 0x00              // 00000002
                                      };

#endif


/**
 * @brief   Scheduler to set the <>
 * @return  none
 */
void schedulerSetEventPB1(){
  // Enter critical state to modify global variable and exit ASAP
  CORE_DECLARE_IRQ_STATE;
  CORE_ENTER_CRITICAL();
  sl_bt_external_signal(1<<PB1_BIT_POS);
  CORE_EXIT_CRITICAL();
}

/**
 * @brief   Scheduler to set the <>
 * @return  none
 */
void schedulerSetEventPB0(){
  // Enter critical state to modify global variable and exit ASAP
  CORE_DECLARE_IRQ_STATE;
  CORE_ENTER_CRITICAL();
  sl_bt_external_signal(1<<PB0_BIT_POS);
  CORE_EXIT_CRITICAL();
}


/**
 * @brief   Scheduler to set the LETIMER0 event where COMP1 condition is met
 * @return  none
 */
void schedulerSetEventLETIMER0Comp1(){
  // Enter critical state to modify global variable and exit ASAP
  CORE_DECLARE_IRQ_STATE;
  CORE_ENTER_CRITICAL();
  SchedulerEvents |= (1<<LETIMERCOMP1_BIT_POS);
  sl_bt_external_signal(1<<LETIMERCOMP1_BIT_POS);
  CORE_EXIT_CRITICAL();
}

/**
 * @brief   Scheduler to set the LETIMER0 event where UF condition is met
 * @return  none
 */
void schedulerSetEventLETIMER0UF(){
  // Enter critical state to modify global variable and exit ASAP
  CORE_DECLARE_IRQ_STATE;
  CORE_ENTER_CRITICAL();
  SchedulerEvents |= (1<<LETIMERUF_BIT_POS);
  sl_bt_external_signal(1<<LETIMERUF_BIT_POS);
  CORE_EXIT_CRITICAL();
}

/**
 * @brief   Scheduler to set the event where I2C transfer is done
 * @return  none
 */
void schedulerSetEventI2CTransferDone(){
  // Enter critical state to modify global variable and exit ASAP
  CORE_DECLARE_IRQ_STATE;
  CORE_ENTER_CRITICAL();
  SchedulerEvents |= (1<<I2C_TRANSFER_COMPLETE_BIT_POS);
  sl_bt_external_signal(1<<I2C_TRANSFER_COMPLETE_BIT_POS);
  CORE_EXIT_CRITICAL();
}


/**
 * @brief   Checks if any event are present to handle and returns them. In case
 *          of multiple events, the most important event shall be handled first.
 * @return  Returns the latest event to handle
 */
uint32_t getNextEvent(){

  // Set the default event to be returned as EVENT_NONE
  uint32_t ReturnEvent = EVENT_NONE;

  /* Determine the event to return based on priority level.
   *
   * Priority level:
   *    EVENT_I2C_TRANSFER_COMPLETE
   *    EVENT_LETIMER_COMP1
   *    EVENT_LETIMER_UF
   */

  if(SchedulerEvents & (1<<I2C_TRANSFER_COMPLETE_BIT_POS))
    ReturnEvent = EVENT_I2C_TRANSFER_COMPLETE;
  else if(SchedulerEvents & (1<<LETIMERCOMP1_BIT_POS))
    ReturnEvent = EVENT_LETIMER_COMP1;
  else if (SchedulerEvents & (1<<LETIMERUF_BIT_POS))
    ReturnEvent = EVENT_LETIMER_UF;

  // Enter critical state to modify global variable and exit ASAP
  CORE_DECLARE_IRQ_STATE;
  CORE_ENTER_CRITICAL();
  switch(ReturnEvent){
    case EVENT_LETIMER_COMP1:
                        SchedulerEvents &= ~(1<<LETIMERCOMP1_BIT_POS);
                        break;
    case EVENT_LETIMER_UF:
                        SchedulerEvents &= ~(1<<LETIMERUF_BIT_POS);
                        break;
    case EVENT_I2C_TRANSFER_COMPLETE:
                        SchedulerEvents &= ~(1<<I2C_TRANSFER_COMPLETE_BIT_POS);
                        break;
  }
  CORE_EXIT_CRITICAL();

  return ReturnEvent;
}

#if BUILD_INCLUDES_BLE_SERVER == 1
/**
 * @brief   State machine to get the temperature from Si7021 chip over I2C using
 *          IRQs and send it via BT
 * @param   evt -   event occurring outside of the state machine using which
 *                  the state machine shall determine the next course of action
 * @return  none
 */
void temperature_state_machine_bt(sl_bt_msg_t *evt){
  State_t currentState;
  static State_t nextState = stateIdle;
  uint32_t temperature_reading = 0;
  uint16_t Si7021_data = 0;
  sl_status_t sc; // status code

  currentState = nextState;
  ble_data_struct_t *bleDataPtr = get_ble_data_ptr();

  // Check the following conditioins and proceed if all are true:
  //  - we have recieved some external event from the bluetooth stack
  //  - the bluetooth connection is open
  //  - indications are turned on by the client
  if((SL_BT_MSG_ID(evt->header) == sl_bt_evt_system_external_signal_id) &&
     (bleDataPtr->connection_open == true) &&
     (bleDataPtr->ok_to_send_htm_indications == true)){
    switch (currentState) {
      case stateIdle:
              nextState = stateIdle; // default
              /*
               * if event is LETIMERUF, then power on the Si7021, set delay for
               * Si7021 POR and go to next state
               */
                if ((evt->data.evt_system_external_signal.extsignals & (1<<LETIMERUF_BIT_POS))){
                    si7021TurnOn();
                    timerWaitUs_irq(SI7021_POR_TIME_US);
                    nextState = waitForSi7021POR;
                }
              break;
      case waitForSi7021POR:
              nextState = waitForSi7021POR; // default
              /*
               * if timer event is complete (denoted by the LETIMER_COMP1 event),
               * then start I2C write operation to request
               * temperature data from the Si7021 chip and go to next state.
               */
                if ((evt->data.evt_system_external_signal.extsignals & (1<<LETIMERCOMP1_BIT_POS))){
                    I2C_Write_Data_itr(SI7021_DEVICE_ADDR, SI7021_CMD_MEASURE_TEMP_NO_HOLD);
                    nextState = waitForI2CWriteTransfer;
                }
              break;
      case waitForI2CWriteTransfer:
              nextState = waitForI2CWriteTransfer; // default
              /*
               * if i2c transfer is done, disable I2C IRQ, setup conversion
               * timer required by the Si7021 chip and go to next state.
               */
                if ((evt->data.evt_system_external_signal.extsignals & (1<<I2C_TRANSFER_COMPLETE_BIT_POS))){
                    NVIC_DisableIRQ(I2C0_IRQn);
                    timerWaitUs_irq(SI7021_14B_CONVERSION_TIME_US);
                    nextState = waitForSi7021Conversion;
                }
              break;
      case waitForSi7021Conversion:
              nextState = waitForSi7021Conversion; // default
              /*
              * if timer event is complete (denoted by the LETIMER_COMP1 event),
              * start I2C read operation to read the requested temperature data
              * from the Si7021 chip and go to next state.
              */
                if ((evt->data.evt_system_external_signal.extsignals & (1<<LETIMERCOMP1_BIT_POS))){
                    I2C_Read_Data_irq(SI7021_DEVICE_ADDR);
                    nextState = waitForI2CReadTransfer;
                }
              break;
      case waitForI2CReadTransfer:
              /*
               * if i2c transfer is done, disable I2C IRQ, retrieve data from
               * I2C read operation and send tempurature data over bluetooth,
               * and go to next state.
               */
              nextState = waitForI2CReadTransfer; // default
                  if ((evt->data.evt_system_external_signal.extsignals & (1<<I2C_TRANSFER_COMPLETE_BIT_POS))){
                    NVIC_DisableIRQ(I2C0_IRQn);
                    si7021TurnOff();
                    uint8_t *p = &htm_temperature_buffer[0];
                    Si7021_data = I2C_Get_Data();

                    // Converting the data received from the sensor into temperature in Celsius
                    // using the formula given in the SI7021 sensor application note AN607
                    temperature_reading = ((Si7021_data*175.72)/65536) - 46.85;

                    // To send via BT, do the following steps:
                    // - update GATT data base with sl_bt_gatt_server_write_attribute_value()
                    // - Convert the temp data into float, insert into the bit
                    //   stream and write into the GATT DB
                    UINT8_TO_BITSTREAM(p, flags);
                    htm_temperature_flt = INT32_TO_FLOAT(temperature_reading*1000, -3);
                    UINT32_TO_BITSTREAM(p, htm_temperature_flt);

                    sc = sl_bt_gatt_server_write_attribute_value(
                          gattdb_temperature_measurement, // handle from gatt_db.h
                          0, // offset
                          5, // length
                          &htm_temperature_buffer[0] // in IEEE-11073 format
                         );

                    //-----------------------------------------------------------------------
                    // call sl_bt_gatt_server_send_indication() ONLY if the following
                    // conditions are met :
                    //  - Connection is open
                    //  - Client has enabled indications for the HTM indications
                    //  - There is no indication currently in-flight
                    //
                    // If all above conditions are met, then update the temperature value on
                    // the LCD display on the row 'DISPLAY_ROW_TEMPVALUE'.
                    // Else, clear the text on the same row
                    //-----------------------------------------------------------------------
                    if  ((bleDataPtr->connection_open == true) &&
                         (bleDataPtr->ok_to_send_htm_indications == true)){

                        if(!((bleDataPtr->indication_in_flight == false)||
                            (get_queue_depth() > 0))){
                            // Server Sending the Indication.
                          sc = sl_bt_gatt_server_send_indication(
                                bleDataPtr->connectionHandle,
                                gattdb_temperature_measurement, // handle from gatt_db.h
                                5,
                                &htm_temperature_buffer[0] // in IEEE-11073 format
                               );

                          if (sc != SL_STATUS_OK) {
                              LOG_ERROR("sl_bt_gatt_server_send_indication() returned != 0 status=0x%04x\r\n", (unsigned int) sc);
                          }
                          bleDataPtr->indication_in_flight = true;

                        } // if
                        else{
                            write_queue(gattdb_temperature_measurement, 5, &htm_temperature_buffer[0]);
                        }
                          displayPrintf(DISPLAY_ROW_TEMPVALUE, "Temp=%d", temperature_reading);
                    }// if
                    else{
                        displayPrintf(DISPLAY_ROW_TEMPVALUE, "");
                    }
                    nextState = stateIdle;
                }// if
              break;
    default:
              break;
    } // switch
  }// end if

  // If the connection has been closed or indication is not given, then we clear
  // the LCD text on the row 'DISPLAY_ROW_TEMPVALUE'.
  if((bleDataPtr->connection_open == false) ||
     (bleDataPtr->ok_to_send_htm_indications == false)){
    displayPrintf(DISPLAY_ROW_TEMPVALUE, "");
  }
} // state_machine()
#endif

#if BUILD_INCLUDES_BLE_CLIENT == 1

void Discovery_State_Machine(sl_bt_msg_t *evt){

    Discovery_States_t currentState;
    static Discovery_States_t nextState = State_1;

    sl_status_t sc; // status code

    currentState = nextState;
    ble_data_struct_t *bleDataPtr = get_ble_data_ptr();
    switch (currentState) {
        case State_1:
                nextState = State_1; // default
                /*
                 * if event is sl_bt_evt_connection_opened_id,
                 *  - call API sl_bt_gatt_discover_primary_services_by_uuid()
                 *    for HTM service
                 */
                  if (SL_BT_MSG_ID(evt->header) == sl_bt_evt_connection_opened_id) {
                    sc = sl_bt_gatt_discover_primary_services_by_uuid(bleDataPtr->connectionHandle,
                                                                      sizeof(thermo_service),
                                                                      (const uint8_t*)&thermo_service[0]);

                    if (sc != SL_STATUS_OK) {
                        LOG_ERROR("sl_bt_gatt_discover_primary_services_by_uuid() returned != 0 status=0x%04x\r\n", (unsigned int) sc);
                    }

                    nextState = State_2;
                  }
                break;
        case State_2:
                nextState = State_2; // default
                /*
                 * if event is sl_bt_evt_gatt_procedure_completed_id,
                 *  - Save the returned service handle for HTM
                 *  - call API sl_bt_gatt_discover_characteristics_by_uuid()
                 *    for HTM characteristics
                 */
                  if (SL_BT_MSG_ID(evt->header) == sl_bt_evt_gatt_procedure_completed_id) {
                      bleDataPtr->serviceHandleHTM = bleDataPtr->serviceHandle;
                      sc = sl_bt_gatt_discover_characteristics_by_uuid(bleDataPtr->connectionHandle,
                                                                       bleDataPtr->serviceHandleHTM,
                                                                        sizeof(thermo_char),
                                                                        (const uint8_t*) &thermo_char[0]);

                      if (sc != SL_STATUS_OK) {
                          LOG_ERROR("sl_bt_gatt_discover_characteristics_by_uuid() returned != 0 status=0x%04x\r\n", (unsigned int) sc);
                      }
                      nextState = State_3;
                  }
                break;
        case State_3:
                nextState = State_3; // default
                 /*
                  * if event is sl_bt_evt_gatt_procedure_completed_id,
                  *  - Save the returned characteristic handle for HTM
                  *  - call API sl_bt_gatt_set_characteristic_notification()
                  *    for HTM characteristics
                  */
                   if (SL_BT_MSG_ID(evt->header) == sl_bt_evt_gatt_procedure_completed_id) {

                       bleDataPtr->characteristicHandleHTM = bleDataPtr->characteristicHandle;
                       sc = sl_bt_gatt_set_characteristic_notification(bleDataPtr->connectionHandle,
                                                                        bleDataPtr->characteristicHandleHTM,
                                                                        sl_bt_gatt_indication);

                       if (sc != SL_STATUS_OK) {
                           LOG_ERROR("sl_bt_gatt_set_characteristic_notification() returned != 0 status=0x%04x\r\n", (unsigned int) sc);
                       }
                       nextState = State_4;
                   }
                break;
        case State_4:
                nextState = State_4; // default
                /*
                 * if event is sl_bt_evt_gatt_procedure_completed_id,
                 *  - update the LCD row DISPLAY_ROW_CONNECTION
                 *  - call API sl_bt_gatt_discover_primary_services_by_uuid()
                 *    for button service
                 */
                  if (SL_BT_MSG_ID(evt->header) == sl_bt_evt_gatt_procedure_completed_id) {
                      displayPrintf(DISPLAY_ROW_CONNECTION, "Handling Indications");
                      sc = sl_bt_gatt_discover_primary_services_by_uuid(bleDataPtr->connectionHandle,
                                                                        sizeof(button_service),
                                                                        (const uint8_t*)&button_service[0]);

                      if (sc != SL_STATUS_OK) {
                          LOG_ERROR("sl_bt_gatt_discover_primary_services_by_uuid() returned != 0 status=0x%04x\r\n", (unsigned int) sc);
                      }
                      nextState = State_5;
                  }
                break;
        case State_5:
                nextState = State_5; // default
                /*
                 * if event is sl_bt_evt_connection_opened_id,
                 *  - Save the returned service handle for Button service
                 *  - call API sl_bt_gatt_discover_characteristics_by_uuid()
                 *    for Button characteristics
                 */
                  if (SL_BT_MSG_ID(evt->header) == sl_bt_evt_gatt_procedure_completed_id) {
                    bleDataPtr->serviceHandleButton = bleDataPtr->serviceHandle;
                    sc = sl_bt_gatt_discover_characteristics_by_uuid(bleDataPtr->connectionHandle,
                                                                     bleDataPtr->serviceHandleButton,
                                                                     sizeof(button_char),
                                                                     (const uint8_t*) &button_char[0]);

                    if (sc != SL_STATUS_OK) {
                        LOG_ERROR("sl_bt_gatt_discover_characteristics_by_uuid() returned != 0 status=0x%04x\r\n", (unsigned int) sc);
                    }

                    nextState = State_6;
                  }
                break;
        case State_6:
                nextState = State_6; // default
                /*
                 * if event is sl_bt_evt_gatt_procedure_completed_id,
                  *  - Save the returned characteristic handle for Button
                  *    characteristics
                  *  - call API sl_bt_gatt_set_characteristic_notification()
                  *    for HTM characteristics
                  *  - Save the status indication state for the button
                  *    characteristics
                 */
                  if (SL_BT_MSG_ID(evt->header) == sl_bt_evt_gatt_procedure_completed_id) {
                     bleDataPtr->characteristicHandleButton = bleDataPtr->characteristicHandle;
                     sc = sl_bt_gatt_set_characteristic_notification(bleDataPtr->connectionHandle,
                                                                      bleDataPtr->characteristicHandleButton,
                                                                      sl_bt_gatt_indication);

                     if (sc != SL_STATUS_OK) {
                         LOG_ERROR("sl_bt_gatt_set_characteristic_notification() returned != 0 status=0x%04x\r\n", (unsigned int) sc);
                     }
                     bleDataPtr->isIndicationOnButton = true;
                     nextState = State_7;
                  }
                  break;
        case State_7:
                nextState = State_7; // default
                /*
                 * if the connection is closed, the start discovery from the beginning
                 */

                if (SL_BT_MSG_ID(evt->header) == sl_bt_evt_connection_closed_id) {
                    nextState = State_1;
                }
                break;

    }
}
#endif
