
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* XDCtools Header files */
#include <xdc/std.h>
#include <xdc/runtime/Log.h>

// XDCtools Header files
#include <xdc/runtime/Error.h>
#include <xdc/runtime/System.h>

/* TI-RTOS Header files */
#include <ti/sysbios/BIOS.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Swi.h>
#include <ti/sysbios/knl/Queue.h>
#include <ti/sysbios/knl/Event.h>
#include <ti/sysbios/knl/Idle.h>
#include <ti/sysbios/knl/Mailbox.h>
#include <ti/sysbios/knl/Clock.h>
#include <ti/sysbios/knl/Semaphore.h>
#include <ti/drivers/GPIO.h>
#include <ti/net/http/httpcli.h>
#include <ti/sysbios/knl/Intrinsics.h>

// new headers
#include <stdbool.h>
#include "inc/hw_memmap.h"
#include "inc/hw_ints.h"
#include "inc/hw_types.h"
#include "driverlib/adc.h"
#include "driverlib/gpio.h"
#include "driverlib/sysctl.h"

#include "Board.h"

#include <sys/socket.h>
#include <arpa/inet.h>


extern Swi_Handle swi0;
extern Mailbox_Handle mailbox0;
extern Task_Handle task0;


extern Semaphore_Handle semaphore0;     // posted by httpTask and pended by clientTask
char   tempstr[50];                     // intensity string
char   time[50];

// ADC code start
// ADC values will be put here
//
uint32_t ADCValues[2];

Void timerHWI(UArg arg1)
{
    //
    // Just trigger the ADC conversion for sequence 3. The rest will be done in SWI
    //
    ADCProcessorTrigger(ADC0_BASE, 3);

    // post the SWI for the rest of ADC data conversion and buffering
    //
    Swi_post(swi0);
}

Void ADCSwi(UArg arg1, UArg arg2)
{
    static uint32_t PE3_value;

    //
    // Wait for conversion to be completed for sequence 3
    //
    while(!ADCIntStatus(ADC0_BASE, 3, false));

    //
    // Clear the ADC interrupt flag for sequence 3
    //
    ADCIntClear(ADC0_BASE, 3);

    //
    // Read ADC Value from sequence 3
    //
    ADCSequenceDataGet(ADC0_BASE, 3, ADCValues);

    //
    // Port E Pin 3 is the AIN0 pin. Therefore connect PE3 pin to the line that you want to
    // acquire. +3.3V --> 4095 and 0V --> 0
    //
    PE3_value = ADCValues[0]; // PE3 : Port E pin 3


    // send the ADC PE3 values to the taskAverage()
    //
    Mailbox_post(mailbox0, &PE3_value, BIOS_NO_WAIT);
}

Void taskAverage(UArg arg1, UArg arg2)
{
    static uint32_t pe3_val, pe3_average, tot=0;
    int i;


    while(1) {
        i=0;

     //   pe3_average = 0;
        tot = 0;                // clear total ADC values
        for(i=0;i<10;i++) {     // 10 ADC values will be retrieved

            // wait for the mailbox until the buffer is full
            //
            Mailbox_pend(mailbox0, &pe3_val, BIOS_WAIT_FOREVER);

            tot += pe3_val;
        }
        pe3_average = tot /10;

        Task_sleep(1000);

        if (pe3_average < 3600 ){
        sprintf(tempstr, "Less than predetermined value: %d nm\n", pe3_average);
        }

        else{
        sprintf(tempstr, "Greater than predetermined value: %d nm\n", pe3_average);
        }

        System_printf("Average light intensity: %d nm\n", pe3_average);
        System_flush();


        Semaphore_post(semaphore0);

    }
}



void initialize_ADC()
{
    // enable ADC and Port E
    //
    SysCtlPeripheralReset(SYSCTL_PERIPH_ADC0);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_ADC0);
    SysCtlPeripheralReset(SYSCTL_PERIPH_GPIOE);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOE);
    SysCtlDelay(10);

    // Select the analog ADC function for Port E pin 3 (PE3)
    //
    GPIOPinTypeADC(GPIO_PORTE_BASE, GPIO_PIN_3);

    // configure sequence 3
    //
    ADCSequenceConfigure(ADC0_BASE, 3, ADC_TRIGGER_PROCESSOR, 0);

    // every step, only PE3 will be acquired
    //
    ADCSequenceStepConfigure(ADC0_BASE, 3, 0, ADC_CTL_CH0 | ADC_CTL_IE | ADC_CTL_END);

    // Since sample sequence 3 is now configured, it must be enabled.
    //
    ADCSequenceEnable(ADC0_BASE, 3);

    // Clear the interrupt status flag.  This is done to make sure the
    // interrupt flag is cleared before we sample.
    //
    ADCIntClear(ADC0_BASE, 3);
}


   // finish

#define USER_AGENT        "HTTPCli (ARM; TI-RTOS)"
#define SOCKETTEST_IP     "192.168.1.36"
#define TASKSTACKSIZE     4096
#define OUTGOING_PORT     5011
#define INCOMING_PORT     5030



/*
 *  ======== printError ========
 */
void printError(char *errString, int code)
{
    System_printf("Error! code = %d, desc = %s\n", code, errString);
    BIOS_exit(code);
}

bool sendData2Server(char *serverIP, int serverPort, char *data, int size)
{
    int sockfd, connStat, numSend;
    bool retval=false;
    struct sockaddr_in serverAddr;

    sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd == -1) {
        System_printf("Socket not created");
        close(sockfd);
        return false;
    }

    memset(&serverAddr, 0, sizeof(serverAddr));  // clear serverAddr structure
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(serverPort);     // convert port # to network order
    inet_pton(AF_INET, serverIP, &(serverAddr.sin_addr));

    connStat = connect(sockfd, (struct sockaddr *)&serverAddr, sizeof(serverAddr));
    if(connStat < 0) {
        System_printf("sendData2Server::Error while connecting to server\n");
    }
    else {
        numSend = send(sockfd, data, size, 0);       // send data to the server
        if(numSend < 0) {
            System_printf("sendData2Server::Error while sending data to server\n");
        }
        else {
            retval = true;      // we successfully sent the temperature string
        }
    }
    System_flush();
    close(sockfd);
    return retval;
}

Void clientSocketTask(UArg arg0, UArg arg1)
{
    while(1) {
        // wait for the semaphore that httpTask() will signal
        // when temperature string is retrieved from api.openweathermap.org site
        //
        Semaphore_pend(semaphore0, BIOS_WAIT_FOREVER);

        GPIO_write(Board_LED0, 1); // turn on the LED

        // connect to SocketTest program on the system with given IP/port
        // send hello message which has a length of 5.
       // strcat(tempstr,time);
        if(sendData2Server(SOCKETTEST_IP, OUTGOING_PORT, tempstr, strlen(tempstr))) {
            System_printf("clientSocketTask:: Data is sent to the server\n");
            System_flush();

        }

        GPIO_write(Board_LED0, 0);  // turn off the LED
    }
}


bool createTasks(void)
{
    static Task_Handle  taskHandle2, taskHandle3;
    Task_Params taskParams;
    Error_Block eb;

    Error_init(&eb);


    Task_Params_init(&taskParams);
    taskParams.stackSize = TASKSTACKSIZE;
    taskParams.priority = 1;
    taskHandle2 = Task_create((Task_FuncPtr)clientSocketTask, &taskParams, &eb);
/*
    Task_Params_init(&taskParams);
    taskParams.stackSize = TASKSTACKSIZE;
    taskParams.priority = 1;
    taskHandle3 = Task_create((Task_FuncPtr) startNTP, &taskParams, &eb);

    if ( taskHandle2 == NULL || taskHandle3 == NULL) {
            printError("netIPAddrHook: Failed to create new Tasks\n", -1);
            return false;
        }
*/

/*
    Task_Params_init(&taskParams);
    taskParams.stackSize = TASKSTACKSIZE;
    taskParams.priority = 1;
    taskHandle3 = Task_create((Task_FuncPtr)serverSocketTask, &taskParams, &eb);

    if ( taskHandle2 == NULL || taskHandle3 == NULL) {
        printError("netIPAddrHook: Failed to create HTTP, Socket and Server Tasks\n", -1);
        return false;
    }
*/
    return true;
}

//Void startNTP(UArg arg1, UArg arg2);


//  This function is called when IP Addr is added or deleted
//

void netIPAddrHook(unsigned int IPAddr, unsigned int IfIdx, unsigned int fAdd)
{
    // Create a HTTP task when the IP address is added
    if (fAdd) {
        createTasks();
    }
}


int main(void)
{
    /* Call board init functions */
    Board_initGeneral();
    Board_initGPIO();
    Board_initEMAC();

    // this function initializes PE3 as ADC input.
       //
       initialize_ADC();



    /* Turn on user LED */
    GPIO_write(Board_LED0, Board_LED_ON);

    System_printf("Starting the HTTP GET example\nSystem provider is set to "
            "SysMin. Halt the target to view any SysMin contents in ROV.\n");
    /* SysMin will only print to the console when you call flush or exit */
    System_flush();


    /* Start BIOS */
    BIOS_start();

    return (0);
}
