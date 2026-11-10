#ifndef UWB_TEST_TASK_H
#define UWB_TEST_TASK_H
void UWB_TestEnterCritical(void);
void UWB_TestExitCritical(void);
#define taskENTER_CRITICAL() UWB_TestEnterCritical()
#define taskEXIT_CRITICAL() UWB_TestExitCritical()
#endif
