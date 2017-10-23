/*
 * AI Speaker program
 * Author  : Ikegami Eri
 * twitter : @e_ruru
 * How to use : http://sendagi3chome.com
 */
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "mbedtls/platform.h"
#include "mbedtls/net.h"
#include "mbedtls/esp_debug.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/certs.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "base64.h"
#include "soc/uart_struct.h"
#include "driver/uart.h"
#include "freertos/queue.h"

/*
 * ### GPIO(General Purpose Input/Output)
 * 録音中にインジケータLEDが点灯(GPIO26 or GPIO18 OUTPUT)
 * ボタンが押されたら録音開始(GPIO26 INPUT)
 *---トラ技オリジナル回路用設定---*
 * ESP32 PinNo.15 : DAC_1, ADC2_CH9, RTC_GPIO7, GPIO26(*), EMAC_RXD0
 * ESP32 PinNo.16 : ADC2_CH7, TOUCH7, RTC_GPIO17, GPIO27(*), EMAC_RX_DV
 * ESP32 PinNo.10 : VDET_1, VRTC, ADC1_CH6(*), RTC_GPIO4, GPIO34
 *
 *---トラ技付録基板用設定1---*
 * ESP32 PinNo.35 : VSPICLK, GPIO18(*), HS1_DATA7
 * ESP32 PinNo.16 : ADC2_CH7, TOUCH7, RTC_GPIO17, GPIO27(*), EMAC_RX_DV
 * ESP32 PinNo.10 : VDET_1, VRTC, ADC1_CH6(*), RTC_GPIO4, GPIO34
 */
#define GPIO_OUTPUT_IO_0    18
//#define GPIO_OUTPUT_IO_0 26
#define GPIO_OUTPUT_PIN_SEL  (1<<GPIO_OUTPUT_IO_0)
#define GPIO_INPUT_IO_0    27
#define GPIO_INPUT_PIN_SEL  (1<<GPIO_INPUT_IO_0)
#define ESP_INTR_FLAG_DEFAULT 0

/*
 * ### ADC(Analog Digital Converter)
 * ADC1のチャンネル6を使用する
 * ESP32 PinNo.10 : VDET_1, ADC1_CH6(*), RTC_GPIO4, GPIO34
 */
#define ADC1_TEST_CHANNEL (6)

/*
 * ### UART(Universal Asyncronous Reciever/Transmitter)
 * ATP3011(音声合成IC)との通信で使用
 *---トラ技オリジナル回路用設定---*
 * ESP32 PinNo.29 :GPIO10,UTx1,SD3
 * ESP32 PinNo.28 :GPIO9 ,URx1,SD2
 *
 *---トラ技付録基板用設定---*
 * ESP32 PinNo.27 :GPIO17,UTx2(*),EMAC_CLK_OUT_180
 * ESP32 PinNo.25 :GPIO16,URx2(*),EMAC_CLK_OUT
 *
 */

//#define ECHO_TEST_TXD  (10)
//#define ECHO_TEST_RXD  (9)
#define ECHO_TEST_TXD  (17)
#define ECHO_TEST_RXD  (16)
#define BUF_SIZE (1024)

/*
 * ### GCP(Google Cloud Platform)
 * Cloud Speech APIのリクエスト用WAVファイルのヘッダーデータ
 * Audio sampling Rate -- 16kHz,2Byte(16bit)
 */
static unsigned char header[44] = {
    0x52, 0x49, 0x46, 0x46, 0x24, 0x7D, 0x00, 0x00,
    0x57, 0x41, 0x56, 0x45, 0x66, 0x6D, 0x74, 0x20,
    0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x80, 0x3E, 0x00, 0x00, 0x00, 0x7D, 0x00, 0x00,
    0x02, 0x00, 0x10, 0x00, 0x64, 0x61, 0x74, 0x61,
    0x00, 0x7D, 0x00, 0x00
};

/* Audio data buffer */
unsigned char audio_test[32044];
static int bufferLength = 32043;

/* BAE64 encode buffer */
unsigned char base64_buffer[42730];

unsigned int bufferCount = 0;
short int analogVal = 0;
size_t len;

/* transcript data buffer */
char transcript[64];

/*　task　ponter　*/
static int *adcEnd, *wifiEnd;

/* ローマ字変換後のデータ入れ物 */
static char romaji[64];

/* LOG tag*/
static const char *TAG = "ESP-WROOM-32 : ";

/*
 * ### Wifi
 * SSID & PASSの設定
 * SpeeechTrancrateSW でリクエストの内容を変える
 */
#define EXAMPLE_WIFI_SSID "DAMMY_SSID"
#define EXAMPLE_WIFI_PASS "PASSWORD"
int SpeeechTrancrateSW = 1;
char REQUEST_SERVER[64], REQUEST_PORT[64];

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
 but we only care about one event - are we connected
 to the AP with an IP? */
const int CONNECTED_BIT = BIT0;

/*
 * ### Google API Access token
 *　How to Get Access token : https://cloud.google.com/docs/authentication?hl=ja
 */
#define AccessToken "ya29.El_tBAN_1ptLuxTSUpd0EqAmxIBS7M4wKzHb0lKDYC4DbXavLxaIUukk879u9GaFm2KuRQRMNFHhaJR3KbWR5PVaPwMcdNP0m3zcQeucz85dkFd8QkyNBekvKXDmN56RbA"

/*
 * ### Cloud Speech API
 * リクエスト用 server,port,URL,json
 * How to use : https://cloud.google.com/speech/?hl=ja
 * languageCode : en-US(*), ja-JP
 */
#define SPEECH_SERVER "speech.googleapis.com"
#define SPEECH_PORT "443"
#define SPEECH_URL "/v1beta1/speech:syncrecognize"
#define SPEECH_JSON_0 "\{\"config\":\{\"encoding\":\"LINEAR16\",\"sampleRate\":16000,\"languageCode\":\"en-US\"\},\"audio\":\{\"content\":\""

static const char *SPEECH_API_REQ_0 =
"POST "SPEECH_URL" HTTP/1.1\r\n"
"Host: "SPEECH_SERVER"\r\n"
"Content-Type: application/json\r\n"
"Authorization: Bearer "AccessToken"\r\n"
"Content-Length: ";
static const char *SPEECH_API_REQ_1 = "\r\n"
"\r\n"
SPEECH_JSON_0;
static const char *SPEECH_API_REQ_2 = "\"\}\}"
"\r\n\r\n";

/*
 * ### Cloud transrate API
 * リクエスト用 server,port,URL,json
 * How to use : https://cloud.google.com/translate/?hl=ja
 * source : en(*), ja
 * target : en, ja(*)
 */
#define TRANSLATE_SERVER "translation.googleapis.com"
#define TRANSRATE_PORT "443"
#define TRANSLATE_URL "/language/translate/v2"
#define TRANSLATE_JSON_0 "\{'q':'"
#define TRANSLATE_JSON_1 "','source':'en','target':'ja','format':'text'\}"

static const char *TRANSLATE_API_REQ_0 =
"POST "TRANSLATE_URL" HTTP/1.1\r\n"
"Host: "TRANSLATE_SERVER"\r\n"
"Content-Type: application/json\r\n"
"Authorization: Bearer "AccessToken"\r\n"
"Content-Length: "
;
static const char *TRANSLATE_API_REQ_1 = "\r\n\r\n"TRANSLATE_JSON_0;
static const char *TRANSLATE_API_REQ_2 = TRANSLATE_JSON_1"\r\n\r\n";

/*
 * ひらがな⇆ローマ字変換dictionary
 * 文字code : UTF-8
 *　頑張れば漢字なんかもおしゃべりさせることができるかもしれない...(手記はここで途絶えている)
 */
#define dicIndex 71
struct convertDic{
    char kana[5];
    char roma[3];
};
struct convertDic dic[dicIndex] = {
    {"あ","a"},{"い","i"},{"う","u"},{"え","e"},{"お","o"},
    {"か","ka"},{"き","ki"},{"く","ku"},{"け","ke"},{"こ","ko"},
    {"が","ga"},{"ぎ","gi"},{"ぐ","gu"},{"げ","ge"},{"ご","go"},
    {"さ","sa"},{"し","si"},{"す","su"},{"せ","se"},{"そ","so"},
    {"ざ","za"},{"じ","zi"},{"ず","zu"},{"ぜ","ze"},{"ぞ","zo"},
    {"た","ta"},{"ち","ti"},{"つ","tu"},{"て","te"},{"と","to"},
    {"だ","da"},{"ぢ","di"},{"づ","du"},{"で","de"},{"ど","do"},
    {"な","na"},{"に","ni"},{"ぬ","nu"},{"ね","ne"},{"の","no"},
    {"は","ha"},{"ひ","hi"},{"ふ","hu"},{"へ","he"},{"ほ","ho"},
    {"ば","ba"},{"び","bi"},{"ぶ","bu"},{"べ","be"},{"ぼ","bo"},
    {"ぱ","pa"},{"ぴ","pi"},{"ぷ","pu"},{"ぺ","pe"},{"ぽ","po"},
    {"ま","ma"},{"み","mi"},{"む","mu"},{"め","me"},{"も","mo"},
    {"や","ya"},{"ゆ","yu"},{"よ","yo"},
    {"ら","ra"},{"り","ri"},{"る","ru"},{"れ","re"},{"ろ","ro"},
    {"わ","wa"},{"を","wo"},{"ん","nn"}
};

/* All function */
static esp_err_t event_handler(void *ctx, system_event_t *event);
static void initialise_wifi(void);
static void echo_task(int voice_num);
void https_get_task(void *pvParameters);
void adc1task(void* arg);

/* Root cert for howsmyssl.com, taken from server_root_cert.pem
 
 The PEM file was extracted from the output of this command:
 openssl s_client -showcerts -connect www.howsmyssl.com:443 </dev/null
 
 The CA root cert is the last cert given in the chain of certs.
 
 To embed it in the app binary, the PEM file is named
 in the component.mk COMPONENT_EMBED_TXTFILES variable.
 */
extern const uint8_t server_root_cert_pem_start[] asm("_binary_server_root_cert_pem_start");
extern const uint8_t server_root_cert_pem_end[]   asm("_binary_server_root_cert_pem_end");

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
        case SYSTEM_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case SYSTEM_EVENT_STA_GOT_IP:
            xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            /* This is a workaround as ESP32 WiFi libs don't currently
             auto-reassociate. */
            esp_wifi_connect();
            xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void initialise_wifi(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_WIFI_SSID,
            .password = EXAMPLE_WIFI_PASS,
        },
    };
    //LOG(TAG, "Setting WiFi configuration SSID %s...", (char)wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
    
}


/*
 * use UART1 port
 * baudRate : 9600bps
 * parity   : none
 * stop bit : 1
 * flow ctrl: none
 */
static void echo_task(int voice_num){
    
    const int uart_num = UART_NUM_2;
    
    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 5500,
    };
    
    uart_param_config(uart_num, &uart_config);
    uart_set_pin(uart_num, ECHO_TEST_TXD, ECHO_TEST_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(uart_num, BUF_SIZE * 2, 0, 0, NULL, 0);
    
    const char data1[19] = {'k','i','k','i','t','o','r','e','m','a','s','e','n','d','e','s','i','t','a'};
    const char data2[9] = {'k','o','n','n','i','t','i','w','a'};
    const char data3[8] = {'k','o','n','b','a','n','w','a'};
    const char data4[14] = {'d','o','u','i','t','a','s','i','m','a','s','i','t','e'};
    const char data5[8] = {'g','u','n','n','m','o','n','i'};
    const char ends[2] = {'\r','\n'};
    switch(voice_num){
        case 0:
            uart_write_bytes(uart_num, (const char*) data1, sizeof(data1));
            break;
        case 1:
            uart_write_bytes(uart_num, (const char*) data2, sizeof(data2));
            break;
        case 2:
            uart_write_bytes(uart_num, (const char*) data3, sizeof(data3));
            break;
        case 3:
            uart_write_bytes(uart_num, (const char*) data4, sizeof(data4));
            break;
        case 4:
            uart_write_bytes(uart_num, (const char*) data5, sizeof(data5));
            break;
        case 5:
            uart_write_bytes(uart_num, (const char*) romaji, sizeof(romaji)-1);
            break;
        default:
            break;
    }
    
    uart_write_bytes(uart_num, (const char*) ends, sizeof(ends));
    vTaskDelay(500 / portTICK_PERIOD_MS);
    
}


void https_get_task(void *pvParameters){

    char buf[512];
    int flags;
    int ret,len;

    /* Speech API or Transrate API switch */
    if(SpeeechTrancrateSW){
        strcpy(REQUEST_SERVER,SPEECH_SERVER);
        strcpy(REQUEST_PORT,SPEECH_PORT);
    }else{
        strcpy(REQUEST_SERVER,TRANSLATE_SERVER);
        strcpy(REQUEST_PORT,TRANSRATE_PORT);
    }
    
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context ssl;
    mbedtls_x509_crt cacert;
    mbedtls_ssl_config conf;
    mbedtls_net_context server_fd;
    
    mbedtls_ssl_init(&ssl);
    mbedtls_x509_crt_init(&cacert);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    
    mbedtls_ssl_config_init(&conf);
    
    mbedtls_entropy_init(&entropy);
    if((ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,NULL, 0)) != 0){
        abort();
    }
    
    
    ret = mbedtls_x509_crt_parse(&cacert, server_root_cert_pem_start,server_root_cert_pem_end-server_root_cert_pem_start);
    
    if(ret < 0){
        abort();
    }
    
    
    /* Hostname set here should match CN in server certificate */
    if((ret = mbedtls_ssl_set_hostname(&ssl, REQUEST_SERVER)) != 0){
        abort();
    }
    
    if((ret = mbedtls_ssl_config_defaults(&conf,MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT)) != 0){
        goto exit;
    }
    
    /* MBEDTLS_SSL_VERIFY_OPTIONAL is bad for security, in this example it will print
     a warning if CA verification fails but it will continue to connect.
     You should consider using MBEDTLS_SSL_VERIFY_REQUIRED in your own code.
     */
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_ca_chain(&conf, &cacert, NULL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
#ifdef CONFIG_MBEDTLS_DEBUG
    mbedtls_esp_enable_debug_log(&conf, 4);
#endif
    
    if ((ret = mbedtls_ssl_setup(&ssl, &conf)) != 0){
        goto exit;
    }
    
    while(1){
        /* Wait for the callback to set the CONNECTED_BIT in the
         event group.
         */
        xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,false, true, portMAX_DELAY);
        printf("Connected to AP");
        
        mbedtls_net_init(&server_fd);
        
        printf("Connecting to %s:%s...", REQUEST_SERVER, REQUEST_PORT);
        
        if ((ret = mbedtls_net_connect(&server_fd, REQUEST_SERVER,REQUEST_PORT, MBEDTLS_NET_PROTO_TCP)) != 0){
            goto exit;
        }
        
        mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);
        
        
        while ((ret = mbedtls_ssl_handshake(&ssl)) != 0){
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE){
                goto exit;
            }
        }
        
        
        if ((flags = mbedtls_ssl_get_verify_result(&ssl)) != 0){
            /* In real life, we probably want to close connection if ret != 0 */
            //ESP_LOGW(TAG, "Failed to verify peer certificate!");
            bzero(buf, sizeof(buf));
            mbedtls_x509_crt_verify_info(buf, sizeof(buf), "  ! ", flags);
        }
        else {
        }
        
        len=0;
        
        if(SpeeechTrancrateSW){
            
            /* ###### Cloud Speech API REQUEST ###### */
            
            char dataSize[] = {'\0'};
            sprintf(dataSize,"%d",42827);
            
            /* HTTP reqest Header */
            while((ret = mbedtls_ssl_write(&ssl, (const unsigned char *)SPEECH_API_REQ_0, strlen(SPEECH_API_REQ_0))) <= 0){
                if(ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE){
                    goto exit;
                }
            }
            
            /* valiable audio data size */
            while((ret = mbedtls_ssl_write(&ssl, &dataSize, strlen(dataSize))) <= 0){
                if(ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE){
                    goto exit;
                }
            }
            /* JSON half first */
            while((ret = mbedtls_ssl_write(&ssl, (const unsigned char *)SPEECH_API_REQ_1, strlen(SPEECH_API_REQ_1))) <= 0){
                if(ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE){
                    goto exit;
                }
            }
            
            len += ret;
            
            /*
             * ### Request content
             *　mbedtls_ssl_writeで送信可能なデータ上限を超えないように分割して送る
             */
            static char req_buffer_size = sizeof(base64_buffer) / 12000;
            static unsigned int req_buffer_size_surpl = sizeof(base64_buffer) % 12000;
            static char req_count = 0;
            
            for(req_count=0; req_count<req_buffer_size; req_count++){
                while((ret = mbedtls_ssl_write(&ssl, &(base64_buffer[12000*req_count]), 12000)) <= 0){
                    if(ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE){
                        goto exit;
                    }
                }
            }
            
            while((ret = mbedtls_ssl_write(&ssl, &(base64_buffer[12000*req_count]), req_buffer_size_surpl-2)) <= 0){
                if(ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE){
                    goto exit;
                }
            }
            
            req_count = 0;
            len += ret;
            
            /* JSON half first */
            while((ret = mbedtls_ssl_write(&ssl, (const unsigned char *)SPEECH_API_REQ_2, strlen(SPEECH_API_REQ_2))) <= 0){
                if(ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE){
                    goto exit;
                }
            }
            
        }else{
            
            /* ###### Cloud Transrate API REQUEST ###### */
            
            char dataSize[] = {'\0'};
            sprintf(dataSize,"%d",(52 + strlen(transcript)));
            
            /* HTTP reqest Header */
            while((ret = mbedtls_ssl_write(&ssl, (const unsigned char *)TRANSLATE_API_REQ_0, strlen(TRANSLATE_API_REQ_0))) <= 0){
                if(ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE){
                    goto exit;
                }
            }
            
            /* valiable transcript data size */
            while((ret = mbedtls_ssl_write(&ssl, &dataSize, strlen(dataSize))) <= 0){
                if(ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE){
                    goto exit;
                }
            }
            
            /* JSON half first */
            while((ret = mbedtls_ssl_write(&ssl, (const unsigned char *)TRANSLATE_API_REQ_1, strlen(TRANSLATE_API_REQ_1))) <= 0){
                if(ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE){
                    goto exit;
                }
            }
            
            /* JSON q = transcript */
            while((ret = mbedtls_ssl_write(&ssl, &transcript, strlen(transcript))) <= 0){
                if(ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE){
                    goto exit;
                }
            }
            
            /* JSON half last */
            while((ret = mbedtls_ssl_write(&ssl, (const unsigned char *)TRANSLATE_API_REQ_2, strlen(TRANSLATE_API_REQ_2))) <= 0){
                if(ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE){
                    goto exit;
                }
            }
        }
        
        len += ret;
        
        printf("%d bytes written", len);
        printf("Reading HTTP response...");
        
        do{
            len = sizeof(buf) - 1;
            bzero(buf, sizeof(buf));
            ret = mbedtls_ssl_read(&ssl, (unsigned char *)buf, len);
            
            if(ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;
            
            if(ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY){
                ret = 0;
                break;
            }
            
            if(ret < 0){
                break;
            }
            
            if(ret == 0){
                break;
            }
            
            len = ret;
            /* Print response directly to stdout as it is read */
            for(int i = 0; i < len; i++){
                putchar(buf[i]);
            }
            
            char *jsonBuffer;
            char translations[64];
            char div[] = "\"";
            
            jsonBuffer = strtok(buf,div);
            
            while(jsonBuffer != NULL){
                
                printf("%s\n",jsonBuffer);
                jsonBuffer = strtok(NULL,div);
                
                if(jsonBuffer != NULL){
                    /* Cloud Speech APIの結果を取り出す */
                    if(strcmp(jsonBuffer,"transcript") == 0){
                        
                        jsonBuffer = strtok(NULL,div);
                        jsonBuffer = strtok(NULL,div);
                        
                        strcpy(transcript,jsonBuffer);
                        
                        if(SpeeechTrancrateSW){
                            SpeeechTrancrateSW = 0;
                            mbedtls_ssl_close_notify(&ssl);
                            
                            /* Cloud Transrate API task start */
                            xTaskCreate(&https_get_task, "https_get_task_tranlate", 8192, NULL, 5, NULL);
                            
                            /* Cloud Speech API task end */
                            vTaskDelete(wifiEnd);
                        }
                    }
                    /* Cloud Transrate APIの結果を取り出す */
                    else if(strcmp(jsonBuffer,"translatedText") == 0){
                        
                        jsonBuffer = strtok(NULL,div);
                        jsonBuffer = strtok(NULL,div);
                        
                        strcpy(translations,jsonBuffer);
                        
                        static int dicCount=0;
                        char sample_buffer[8];
                        
                        sample_buffer[3] = '\0';
                        //printf("TEST 00 : %s\n", sample_buffer);
                        for(int i=0;i<(strlen(translations)/3);i++){
                            
                            strncpy(sample_buffer,translations+(i*3),3);
                            sample_buffer[3] = '\0';
                            
                            printf("R");
                            for(dicCount=0;dicCount<dicIndex;dicCount++){
                                if(strcmp(sample_buffer,dic[dicCount].kana) == 0){
                                    strcat(romaji,dic[dicCount].roma);
                                }
                            }
                            dicCount = 0;
                        }
                        
                        /* ATP3011 play Audio */
                        echo_task(5);
                        
                    }
                }
            }
        } while(1);
        
        mbedtls_ssl_close_notify(&ssl);
        
    exit:
        mbedtls_ssl_session_reset(&ssl);
        mbedtls_net_free(&server_fd);
        
        if(ret != 0){
            mbedtls_strerror(ret, buf, 100);
        }
    }
}


char flag = 1;

void adc1task(void* arg)
{
    
    while(1){
        
        for (unsigned int loop_index=0; loop_index<1300; loop_index++){};
        
        if(bufferCount <= bufferLength){
            
            gpio_set_level(GPIO_OUTPUT_IO_0, 1);
            analogVal = adc1_get_voltage(ADC1_TEST_CHANNEL);
            audio_test[51 + bufferCount] = (unsigned char)(analogVal>>2);
            bufferCount++;
            audio_test[51 + bufferCount] = (unsigned char)(analogVal<<2);
            bufferCount++;
            
        }else if(flag && bufferCount>=bufferLength+1){
            gpio_set_level(GPIO_OUTPUT_IO_0, 0);
            
            mbedtls_base64_encode(base64_buffer, sizeof(base64_buffer)+1, &len, audio_test, sizeof(audio_test));
            
            flag = 0;
            
        }else{
            
            ESP_ERROR_CHECK( nvs_flash_init() );
            initialise_wifi();
            
            /* Cloud Speech API task start */
            xTaskCreate(&https_get_task, "https_get_task", 8192, NULL, 2, wifiEnd);
            
            /* Audio REC task end */
            vTaskDelete(adcEnd);
            
        }
        
    }
    
}


void app_main()
{
    /* GPIO output LED setting */
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
    
    /* GPIO input BUTTON setting */
    io_conf.intr_type = GPIO_PIN_INTR_POSEDGE;
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);

    /* initialize ADC */
    adc1_config_width(ADC_WIDTH_12Bit);
    adc1_config_channel_atten(ADC1_TEST_CHANNEL,ADC_ATTEN_11db);
    printf("The adc1 value:%d\n",adc1_get_voltage(ADC1_TEST_CHANNEL));
    
    
    for(int i = 0; i<sizeof(header); i++ ){
        
        audio_test[i] = header[i];
        
    }
    
    /* start adc task (RECORDING) */
    xTaskCreate(adc1task, "adc1task", 2048, NULL, 1, adcEnd);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    
    
}
