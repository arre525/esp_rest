/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain
 * this notice you can do whatever you want with this stuff. If we meet some day,
 * and you think this stuff is worth it, you can buy me a beer in return.
 * ----------------------------------------------------------------------------
 * minor cleanup by Lukasz Piatkowski
 * used in esp_rest project
 * repo: https://github.com/piontec/esp_rest
 */

#include "ets_sys.h"
#include "osapi.h"
#include "espmissingincludes.h"
#include "c_types.h"
#include "user_interface.h"
#include "espconn.h"
#include "mem.h"
#include "gpio.h"
#include "common.h"

#define MAXTIMINGS 10000
#define BREAKTIME 20
#define MAXTRY 200000
#define DHT_PIN 0

float * ICACHE_FLASH_ATTR readDHT(void)
{
    static float r[2];
    int counter = 0;
    int laststate = 1;
    int i = 0;
    int j = 0;
    int checksum = 0;
    memset(r,0,sizeof(r));

    int data[5];

    data[0] = data[1] = data[2] = data[3] = data[4] = 0;

    GPIO_OUTPUT_SET(DHT_PIN, 1);
    os_delay_us(250000);
    GPIO_OUTPUT_SET(DHT_PIN, 0);
    os_delay_us(20000);
    GPIO_OUTPUT_SET(DHT_PIN, 1);
    os_delay_us(40);
    GPIO_DIS_OUTPUT(DHT_PIN);
    PIN_PULLUP_EN(PERIPHS_IO_MUX_GPIO0_U);


    // wait for pin to drop?
    while (GPIO_INPUT_GET(DHT_PIN) == 1 && i<MAXTRY) {
        os_delay_us(1);
        i++;
    }

    if(i==MAXTRY) {
        os_printf ("ERROR: GPIO wait error\n");
        return r;
    }

    // read data!

    for (i = 0; i < MAXTIMINGS; i++) {
        counter = 0;
        while ( GPIO_INPUT_GET(DHT_PIN) == laststate) {
            counter++;
            os_delay_us(1);
            if (counter == 1000)
                break;
        }
        laststate = GPIO_INPUT_GET(DHT_PIN);
        if (counter == 1000) break;

        //bits[bitidx++] = counter;

        if ((i>3) && (i%2 == 0)) {
            // shove each bit into the storage bytes
            data[j/8] <<= 1;
            if (counter > BREAKTIME)
                data[j/8] |= 1;
            j++;
        }
    }


    float temp_p, hum_p;
    if (j >= 39) {
        checksum = (data[0] + data[1] + data[2] + data[3]) & 0xFF;
        if (data[4] == checksum) {
            // yay! checksum is valid

            hum_p = data[0] + ((float) data[1])/100;

            temp_p = data[2] + ((float) data[3])/100;

            r[0] = temp_p;
            r[1] = hum_p;

            debug_print("Checksum was %d\n",checksum);

        }
        else
        	debug_print("ERROR: DHT22 checksum is not correct\n");
    }
    else
    	debug_print("ERROR: j < 39");
    return r;
}



void ICACHE_FLASH_ATTR DHT(void) {
    readDHT();
}


void DHTInit() {
    //Set GPIO2 to output mode for DHT22
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U, FUNC_GPIO0);
    PIN_PULLUP_EN(PERIPHS_IO_MUX_GPIO0_U);
}
