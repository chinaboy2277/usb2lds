/*
 * NLDSBB_firmware.c
 *
 * Created: 19/02/2014 22:11:16
 *  Author: Xevel
 */ 

 #define F_CPU 16000000 // HACK 16MHz crystal instead of // 8MHz internal clock (CKDIV8 is deactivated)

#include <avr/io.h>
#include <avr/sfr_defs.h>
#include <avr/power.h>
#include <avr/wdt.h>
#include <avr/interrupt.h>
#include <util/delay.h>

#define bitSet(value, bit)       ((value) |= (1UL << (bit)))
#define bitClear(value, bit)     ((value) &= ~(1UL << (bit)))

volatile uint16_t volatile_rpm;
volatile uint8_t rpm_updated;

#define DEBUG   (1)

#if DEBUG 
#define dbinit1     bitSet(DDRD , 2)
#define dbup1       bitSet(PORTD , 2)
#define dbdown1     bitClear(PORTD , 2)
#define dbtoggle1   bitSet(PIND , 2)
#else
#define dbinit1     
#define dbup1       
#define dbdown1     
#define dbtoggle1   
#endif

// Pin used:
//   USART RX (PD0/RXD)
//   MOT PWM (PD5/OC0B) inverted logic
//   TX LED (PD6)
//   /MOT (PB1)


void tx_led_on(){
    bitSet(PORTD, 6);
}
void tx_led_off(){
    bitClear(PORTD, 6);
}
void tx_led_toggle(){
    bitSet(PIND, 6);    
}

// PB1 is the /MOT input. Motor control is done only if this pin is set to LOW or left floating (it has a pull-down).
uint8_t mot_should_run(){
    return bit_is_clear(PORTB, 1);
}

uint16_t rpm;
uint8_t has_new_rpm_value;

void usart_setup(){
    cli();
    UCSRA = (1<<U2X); // x2
    UBRRL = 16; // cf datasheet p144
    UCSRB =  (1<<RXCIE) | (1 << RXEN) | (1 << TXEN);  // enable RX and its interrupt
    sei();
}

void usart_write(uint8_t data){ // for debug, the pin is left unconnected
loop_until_bit_is_set(UCSRA, UDRE);
UDR = data;
}

int16_t rpm_setpoint = 19200; //speed is expressed in 64th of rpm (6 bit shift) and we want it to turn at 300 rpm (= 5 rotation/s)
uint8_t motor_pwm = 200;

float Kp = 0.004;

void apply_motor_pwm(uint8_t val){
    OCR0B = val;
}


int main(void)
{
    // disable watchdog timer
    MCUSR &= ~(1 << WDRF);
    wdt_disable();

    dbinit1;
    dbdown1;

    // disable clock division : clock is now at the undivided 8MHz 
    clock_prescale_set(clock_div_1);

    // setup TX LED (PD6)
    bitSet(DDRD, 6);

    // Setup motor PWM (PD5/OC0B), inverting logic, phase correct (mode 1)
    TCCR0A = (1 << COM0B1) | (0 << COM0B0) | (0 << WGM01) | (1 << WGM00);
    TCCR0B = (0 << WGM02) | (1 << CS02) | (0 << CS01) | (0 << CS00);
    apply_motor_pwm(motor_pwm);
    bitSet(DDRD, 5);

    // Setup USART at 115200 bps, only RX
    usart_setup();
    sei();
    uint8_t nb_cnt = 0;
    while(1)
    {
        cli();
        if (rpm_updated){
            rpm = volatile_rpm;
            rpm_updated = 0;
            has_new_rpm_value = 1;
            
            nb_cnt++;
            //dbup1;
            //dbdown1;
        }
        sei();
        if (has_new_rpm_value && nb_cnt == 60){
            //usart_write(rpm >> 7);
            nb_cnt = 0;
            has_new_rpm_value = 0;
            int16_t err = rpm_setpoint - rpm; // error, in 64th of RPM
            
            float new_mot_pwm = (float)motor_pwm + Kp * err;

            //usart_write((int8_t)(err/128) );
            
            if (new_mot_pwm < 0){
                new_mot_pwm = 0;
            } else if (new_mot_pwm > 255){
                new_mot_pwm = 255;
            }
            motor_pwm = (uint8_t) new_mot_pwm;
            usart_write((uint8_t)err);
            usart_write((uint8_t)(err>>8));
        }
        
        if ( mot_should_run() ){
            apply_motor_pwm(motor_pwm);
        } else {
            apply_motor_pwm(0);
        }

    }
}


typedef enum {
    SEARCH_START = 0,
    GET_ID = 1,
    GET_RPM_L = 2,
    GET_RPM_H = 3,
    GET_DATA_L = 4,
    GET_DATA_H = 5,
    GET_CHECKSUM_L = 6,
    GET_CHECKSUM_H = 7
} state_t ;

state_t state;
uint8_t id;
uint32_t accumulator;
uint16_t checksum;
uint8_t nb_data;
uint16_t rpm_tmp;
uint16_t data;

ISR(USART_RX_vect)
{
    //dbup1;
    uint8_t c = UDR;
    sei(); // once incoming data hs been read, we can release the interrupts

    // parse to find the speed the LDS is turning at
    switch(state){
        case SEARCH_START:
            if (c == 0xFA){
                state = GET_ID;
            }
            break;
        case GET_ID:
            if ( c >= 0xA0 && c <= 0xF9 ){ // ID is supposed to be in this bracket
                id = c;
                accumulator = 0xFA + ((uint16_t)id << 8);
                state = GET_RPM_L;
            } else if ( c != 0xFA ){ // if it's not and it's not the start byte either, go back to the start
                state = SEARCH_START;
            } // else it's the start byte and we stay in the same state
            break;
        case GET_RPM_L:
            rpm_tmp = c;
            state = GET_RPM_H;
            break;
        case GET_RPM_H:
            rpm_tmp |= (uint16_t)c << 8;
            nb_data = 2;
            accumulator = (accumulator << 1) + rpm_tmp;
            state = GET_DATA_L;
            break;
        case GET_DATA_L:
            data = c;
            state = GET_DATA_H;
            break;
        case GET_DATA_H:
            data |= (uint16_t)c << 8;
            accumulator = (accumulator << 1) + data;

            nb_data++;
            if (nb_data >= 10){
                state = GET_CHECKSUM_L;
            } else {
                state = GET_DATA_L;
            }
            break;
        case GET_CHECKSUM_L:
            checksum = c;
            state = GET_CHECKSUM_H;
            break;
        case GET_CHECKSUM_H:
            checksum |= ((uint16_t)c << 8);
            
            accumulator = (accumulator & 0x7FFF) + (accumulator >> 15);
            accumulator = accumulator & 0x7FFF;

            if (accumulator == checksum){
                volatile_rpm = rpm_tmp;
                rpm_updated = 1;
            }
            state = SEARCH_START;
            break;
        default:
            break;
    }

    //for (uint8_t i = 0; i < state; i++){
        //dbdown1;
        //dbup1;
    //}
    
    //dbdown1;
}



