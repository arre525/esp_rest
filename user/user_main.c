// This file was written by Lukasz Piatkowski
// [piontec -at- the well known google mail]
// and is distributed under GPL v2.0 license
// repo: https://github.com/piontec/esp_rest

#include "common.h"
#include "ets_sys.h"
#include "osapi.h"
#include "gpio.h"
#include "os_type.h"
#include "user_config.h"
#include "user_interface.h"
#include "private_user_config.h"
#include "dht22.h"
#include "uart_hw.h"
#include "c_types.h"
#include "mem.h"
#include "ip_addr.h"
#include "espconn.h"
#include "config_store.h"

#define user_procTaskPrio        0
#define user_procTaskQueueLen    1


os_event_t    user_procTaskQueue[user_procTaskQueueLen];
static void user_procTask(os_event_t *events);

static volatile ETSTimer sensor_timer;
static volatile ETSTimer deepsleeptimer;
static volatile ETSTimer checkwifitmr;
static volatile ETSTimer dhtmeasurementtmr;
int lastTemp, lastHum;
int meascount = 0;
int stopmeasuring = 0;
char lastTempTxt [8];
char lastHumTxt [8];
char payload[512];
static int resetCnt=0;
static int totalCnt=0;
static config_t* s_conf;

static void ICACHE_FLASH_ATTR enable_sensors()
{
    //Set LEDGPIO to HIGH
    gpio_output_set((1<<LEDGPIO), 0, (1<<LEDGPIO), 0);
    debug_print ("sensors enabled\n");
}


static void ICACHE_FLASH_ATTR disable_sensors()
{
    //Set LEDGPIO to LOW
    gpio_output_set(0, (1<<LEDGPIO), (1<<LEDGPIO), 0);
    debug_print ("sensors disabled\n");
}

static void ICACHE_FLASH_ATTR convertToText (int dhtReading, char *buf, uint8 maxLength, uint8 decimalPlaces)
{    
    if (dhtReading == 0)
        strncpy(buf, "0.00", 4);
    char tmp [8];
    os_bzero (tmp, 8);
    os_sprintf (tmp, "%d", dhtReading);

    uint8 len = os_strlen (tmp);
    uint8 beforeSep = len - decimalPlaces;
    os_strncpy(buf, tmp, beforeSep);
    buf [beforeSep] = '.';
    os_strncpy (buf + beforeSep + 1, tmp + beforeSep, decimalPlaces);
    buf [len + 1] = 0;
}

static void ICACHE_FLASH_ATTR read_DHT22()
{
    int retry = 0;
    float *r;
    int currtemp;
    int currhum;

    do {
        if (retry++ > 0)
        {
        	debug_print("DHT22 read fail, retrying, try %d/%d\n", retry, MAX_DHT_READ_RETRY);
            os_delay_us(DHT_READ_RETRY_DELAY_US);
        }
        r = readDHT(); // Too many delays cause the esp to reset.. Cant do this a few times..
    }
    while ((r[0] == 0 && r[1] == 0) && retry < MAX_DHT_READ_RETRY);

    if(!(r[0] == 0 && r[1] == 0))
    {
		debug_print("DHT read done\n");

		currtemp=(int)(r[0] * 100);
		currhum=(int)(r[1] * 100);
		convertToText (currtemp, lastTempTxt, 8, 2);
		convertToText (currhum, lastHumTxt, 8, 2);
		debug_print ("Temp = %s *C, Hum = %s %\n", lastTempTxt, lastHumTxt);
		lastTemp += currtemp;
		lastHum += currhum;
		meascount++;
    }
    else
    {
    	debug_print ("Nothing measured\n");
    	currtemp=(int)0;
    	currhum=(int)0;
    }
}


static void ICACHE_FLASH_ATTR sensor_deepsleeptimer_func(void *arg)
{
	system_deep_sleep(INTERVAL_S*DEEPSLEEPSECONDS);
	os_delay_us(100000); // This is important. If not, the device just resets
}


static void ICACHE_FLASH_ATTR at_tcpclient_sent_cb(void *arg)
{
    debug_print("TCP sent callback\n");
    //struct espconn *pespconn = (struct espconn *)arg;
    //espconn_disconnect(pespconn);
    // disable sensors power
    //disable_sensors();
    // goto sleep ; gpio16 -> RST -- requires soldiering on ESP-03

    //debug_print ("going to deep sleep for %ds\n", INTERVAL_S);
    //system_deep_sleep(INTERVAL_S*1000*1000);
}

static void ICACHE_FLASH_ATTR at_tcpclient_discon_cb(void *arg) {
    debug_print("TCP disconnect callback\n");
    struct espconn *pespconn = (struct espconn *)arg;
    os_free(pespconn->proto.tcp);

    os_free(pespconn);
   
    debug_print ("going to deep sleep\n");
    sensor_deepsleeptimer_func(arg);
}


static void ICACHE_FLASH_ATTR at_tcpclient_reconnect_cb(void *arg, sint8 errType)
{
    struct espconn *pespconn = (struct espconn *) arg;
    debug_print("Reconnect callback - resending with delay...\n");
    os_delay_us(1*1000*1000);
    debug_print ("now.\n");
    espconn_sent(pespconn, payload, strlen(payload));
    debug_print("resend done.\n");
}

static void ICACHE_FLASH_ATTR at_tcpclient_read_cb(void *arg, char *data, unsigned short len) 
{
    char buf[128];
    debug_print("Rcv callback: %d\n", os_strlen(data));
    debug_print("%s\n", data);
    struct espconn *conn = arg;
    espconn_disconnect(conn);    
}

static void ICACHE_FLASH_ATTR at_tcpclient_connect_cb(void *arg)
{
    struct espconn *pespconn = (struct espconn *)arg;
    
    debug_print("TCP client connect\n");

    espconn_regist_sentcb(pespconn, at_tcpclient_sent_cb);
    espconn_regist_disconcb(pespconn, at_tcpclient_discon_cb);
    espconn_regist_recvcb(pespconn, at_tcpclient_read_cb);
    os_sprintf(payload, "GET http://api.thingspeak.com/update?api_key=%s&field1=%s&field2=%s HTTP/1.1\r\nHost: api.thingspeak.com\r\nUser-agent: the best\r\nConnection: close\r\n\r\n", THINGSPEAK_KEY, lastTempTxt, lastHumTxt);
    espconn_sent(pespconn, payload, strlen(payload));
}


/* DNS DONE */
static void ICACHE_FLASH_ATTR send_data2(ip_addr_t *ipaddr)
{
    struct espconn *pCon = (struct espconn *)os_zalloc(sizeof(struct espconn));
    if (pCon == NULL)
    {
        os_printf("Error: TCP connect failed - memory allocation for conn failed\n");
        return;
    }
    pCon->type = ESPCONN_TCP;
    pCon->state = ESPCONN_NONE;
    pCon->proto.tcp = (esp_tcp *)os_zalloc(sizeof(esp_tcp));
    if (pCon->proto.tcp == NULL)
    {
        os_printf("Error: TCP connect failed - memory allocation for TCP failed\n");
        return;
    }
    pCon->proto.tcp->local_port = espconn_port();
    pCon->proto.tcp->remote_port = 80;
    //pCon->proto.tcp->remote_port = lwip_htons(80);

    os_memcpy(pCon->proto.tcp->remote_ip, ipaddr, 4);

    espconn_regist_connectcb(pCon, at_tcpclient_connect_cb);
    espconn_regist_reconcb(pCon, at_tcpclient_reconnect_cb);
    debug_print("TCP connecting...\n");
    espconn_connect(pCon);
}


LOCAL void ICACHE_FLASH_ATTR
dnsfound(const char *name, ip_addr_t *ipaddr, void *arg)
{
    struct espconn *pespconn = (struct espconn *)arg;

    if (ipaddr == NULL) {
    	os_printf("Error: Failed resolving ip\n");

        return;
    }

    os_printf("IP found %d.%d.%d.%d\n",
            *((uint8 *)&ipaddr->addr), *((uint8 *)&ipaddr->addr + 1),
            *((uint8 *)&ipaddr->addr + 2), *((uint8 *)&ipaddr->addr + 3));

    send_data2(ipaddr);

}

static void ICACHE_FLASH_ATTR send_data()
{
    struct espconn *pCon = (struct espconn *)os_zalloc(sizeof(struct espconn));
    static ip_addr_t tempip ;
    if (pCon == NULL)
    {
        os_printf("Error: TCP connect failed - memory allocation for conn failed\n");
        return;
    }
    pCon->type = ESPCONN_TCP;
    pCon->state = ESPCONN_NONE;
    espconn_gethostbyname(pCon, "api.thingspeak.com", &tempip, dnsfound);
}

static void ICACHE_FLASH_ATTR doDHTmeasurement(void *arg)
{
	if(meascount < 100 && stopmeasuring != 1)
	{
		read_DHT22();
		// read pressure
		// send data

		if(1)
		{
			os_timer_disarm(&dhtmeasurementtmr);
			os_timer_setfn(&dhtmeasurementtmr, doDHTmeasurement, NULL); /* Assuming you can reschedule your own timer */
			os_timer_arm(&dhtmeasurementtmr, 300, 0);
		}
	}
}




static void ICACHE_FLASH_ATTR initialize_gpio()
{
    // Initialize the GPIO subsystem.
    gpio_init();

    //Set GPIO0 to output mode
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U, FUNC_GPIO0);
    //Set GPIO0 low
    PIN_PULLUP_EN(PERIPHS_IO_MUX_GPIO0_U);
    gpio_output_set(1, 0, 1, 0);

}

static void ICACHE_FLASH_ATTR initialize_uart()
{
    //Set baud rate and other serial parameters to 115200,n,8,1
    uart_div_modify(0, UART_CLK_FREQ/BIT_RATE_115200);
    WRITE_PERI_REG(UART_CONF0(0), (STICK_PARITY_DIS)|(ONE_STOP_BIT << UART_STOP_BIT_NUM_S)| \
                   (EIGHT_BITS << UART_BIT_NUM_S));
    //Reset tx & rx fifo
    SET_PERI_REG_MASK(UART_CONF0(0), UART_RXFIFO_RST|UART_TXFIFO_RST);
    CLEAR_PERI_REG_MASK(UART_CONF0(0), UART_RXFIFO_RST|UART_TXFIFO_RST);
    //Clear pending interrupts
    WRITE_PERI_REG(UART_INT_CLR(0), 0xffff);
}



static void ICACHE_FLASH_ATTR wifirdy_senddata()
{
    os_printf ("Starting normal mode\n");
    debug_print ("Wifi is ready. Do a last measurement and send data\n");

    /* We assume this function cannot run concurrently with another timer! In senddata however, we sleep again, so avoid other samples */
    stopmeasuring = 1;

    if(lastTemp !=0 && lastHum != 0 && meascount > 0)
    {
    	/* Average all results out */
    	lastTemp = lastTemp / meascount;
    	lastHum = lastHum / meascount;
    	convertToText (lastTemp, lastTempTxt, 8, 2);
		convertToText (lastHum, lastHumTxt, 8, 2);
		debug_print ("-- FINAL -- Temp = %s *C, Hum = %s %\n", lastTempTxt, lastHumTxt);
    	send_data(); // Will go to sleep internally if successful
    }
    else
    {
    	sensor_deepsleeptimer_func(0);  // Failed measurement. Sleep;
    }
}


static void ICACHE_FLASH_ATTR CheckWifiTmr(void *arg)
{
	if(wifi_station_get_connect_status() == STATION_GOT_IP)
	{
		os_timer_disarm(&checkwifitmr);
		wifirdy_senddata();
	}
	else
	{
		/* Wait for the next try */
		debug_print("Not yet got an IP\n");
	}
}

static void ICACHE_FLASH_ATTR resetInit()
{
	/* Make sure we always go into deep sleep, whatever happens */
    os_timer_disarm(&deepsleeptimer);
    os_timer_setfn(&deepsleeptimer, (os_timer_func_t *)sensor_deepsleeptimer_func, NULL);
    os_timer_arm(&deepsleeptimer, DEEPSLEEPTIMEOUTMS, 0);

    initialize_gpio();
    s_conf = config_init();
    os_printf("\n\nSoftware version: %s, config version: %d\n", SOFT_VERSION, CONFIG_VERSION);
    debug_print("Current wifi connect status: %d\n", wifi_station_get_connect_status());
    debug_print("Current DHCP status: %d\n", wifi_station_dhcpc_status());
    debug_print("Current wifi mode: %d\n", wifi_get_phy_mode());

    /*
     * While the wifi is getting up, we poll 2x a second to see if it got there
     * In the mean time, we do as many DHT measurements as we can, to average out any weirdness that may come out of it.
     * Once the wifi is up, we take one final measurement, before sending everything through (unless we went into deep sleep by then)
     *
     * The only thing to watch out for, is that we have two concurrent timers. The one for wifi checking, and the one for DHT measurements
     * I would find it surprising if one can interrupt another, but we'll build in a safety just to make sure
     */

    os_timer_disarm(&checkwifitmr);
    os_timer_setfn(&checkwifitmr, CheckWifiTmr, NULL);
    os_timer_arm(&checkwifitmr, 500, 1);

    DHTInit();

    /* We have to use timers for the measurements, since ending a timer function seems to be the only way to let the other code run, and avoid watchdogs */


    os_timer_disarm(&dhtmeasurementtmr);
    os_timer_setfn(&dhtmeasurementtmr, doDHTmeasurement, NULL);
    os_timer_arm(&dhtmeasurementtmr, 300, 0);
}

void ICACHE_FLASH_ATTR init_done()
{
    debug_print("INIT IS DONE!\n");
    resetInit();
}

//Init function
void ICACHE_FLASH_ATTR user_init()
{
    initialize_uart();
    system_init_done_cb(init_done); 
    //resetInit ();
}
