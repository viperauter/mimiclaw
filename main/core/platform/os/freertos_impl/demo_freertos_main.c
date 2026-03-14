#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include <stdio.h>

static SemaphoreHandle_t xSyncSemaphore = NULL;
static SemaphoreHandle_t xCounterMutex = NULL;
static int sharedCounter = 0;

static void vTask1(void *pvParameters)
{
    (void)pvParameters;

    for (;;)
    {
        /* Wait for the semaphore */
        xSemaphoreTake(xSyncSemaphore, portMAX_DELAY);
        
        /* Increment and print the shared counter */
        xSemaphoreTake(xCounterMutex, portMAX_DELAY);
        int currentTick = sharedCounter++;
        xSemaphoreGive(xCounterMutex);
        
        printf("[Task1] tick %d\n", currentTick);
        
        /* Give the semaphore to Task2 */
        xSemaphoreGive(xSyncSemaphore);
        
        /* Small delay to simulate work */
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void vTask2(void *pvParameters)
{
    (void)pvParameters;

    for (;;)
    {
        /* Wait for the semaphore */
        xSemaphoreTake(xSyncSemaphore, portMAX_DELAY);
        
        /* Increment and print the shared counter */
        xSemaphoreTake(xCounterMutex, portMAX_DELAY);
        int currentTick = sharedCounter++;
        xSemaphoreGive(xCounterMutex);
        
        printf("[Task2] tick %d\n", currentTick);
        
        /* Give the semaphore back to Task1 */
        xSemaphoreGive(xSyncSemaphore);
        
        /* Small delay to simulate work */
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

int main(void)
{
    printf("[freertos_demo] starting scheduler...\n");

    /* Create synchronization semaphores */
    xSyncSemaphore = xSemaphoreCreateBinary();
    xCounterMutex = xSemaphoreCreateMutex();
    
    if (xSyncSemaphore == NULL || xCounterMutex == NULL)
    {
        printf("[freertos_demo] Failed to create semaphores\n");
        return 1;
    }

    /* Create the tasks */
    xTaskCreate(vTask1, "Task1", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 1, NULL);
    xTaskCreate(vTask2, "Task2", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 1, NULL);

    /* Give the semaphore to start the synchronization */
    xSemaphoreGive(xSyncSemaphore);

    vTaskStartScheduler();

    /* Should never reach here. */
    printf("[freertos_demo] scheduler exited unexpectedly\n");
    return 0;
}

