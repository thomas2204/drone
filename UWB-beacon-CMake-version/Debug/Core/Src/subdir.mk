################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Core/Src/beacon_twr.c \
../Core/Src/deca_probe_interface.c \
../Core/Src/main.c \
../Core/Src/stm32f1xx_hal_msp.c \
../Core/Src/stm32f1xx_it.c \
../Core/Src/syscalls.c \
../Core/Src/sysmem.c \
../Core/Src/system_stm32f1xx.c \
../Core/Src/uwb_port.c 

OBJS += \
./Core/Src/beacon_twr.o \
./Core/Src/deca_probe_interface.o \
./Core/Src/main.o \
./Core/Src/stm32f1xx_hal_msp.o \
./Core/Src/stm32f1xx_it.o \
./Core/Src/syscalls.o \
./Core/Src/sysmem.o \
./Core/Src/system_stm32f1xx.o \
./Core/Src/uwb_port.o 

C_DEPS += \
./Core/Src/beacon_twr.d \
./Core/Src/deca_probe_interface.d \
./Core/Src/main.d \
./Core/Src/stm32f1xx_hal_msp.d \
./Core/Src/stm32f1xx_it.d \
./Core/Src/syscalls.d \
./Core/Src/sysmem.d \
./Core/Src/system_stm32f1xx.d \
./Core/Src/uwb_port.d 


# Each subdirectory must supply rules for building sources it contributes
Core/Src/%.o Core/Src/%.su Core/Src/%.cyclo: ../Core/Src/%.c Core/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m3 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F103xE -c -I../Core/Inc -I"C:/Users/Matthew/STM32CubeIDE/workspace_2.2.0/Beacon/Core/Inc/dwt_uwb_driver/lib/qmath/include" -I"C:/Users/Matthew/STM32CubeIDE/workspace_2.2.0/Beacon/Core/Inc/dwt_uwb_driver" -I"C:/Users/Matthew/STM32CubeIDE/workspace_2.2.0/Beacon/Core/Inc/dwt_uwb_driver/dw3000" -I"C:/Users/Matthew/STM32CubeIDE/workspace_2.2.0/Beacon/Core/Inc/dwt_uwb_driver/lib/qmath" -I"C:/Users/Matthew/STM32CubeIDE/workspace_2.2.0/Beacon/Core/Inc/dwt_uwb_driver/lib/qmath" -I"C:/Users/Matthew/STM32CubeIDE/workspace_2.2.0/Beacon/Core/Inc/dwt_uwb_driver/dw3720" -I../Drivers/STM32F1xx_HAL_Driver/Inc -I../Drivers/STM32F1xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F1xx/Include -I../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfloat-abi=soft -mthumb -o "$@"

clean: clean-Core-2f-Src

clean-Core-2f-Src:
	-$(RM) ./Core/Src/beacon_twr.cyclo ./Core/Src/beacon_twr.d ./Core/Src/beacon_twr.o ./Core/Src/beacon_twr.su ./Core/Src/deca_probe_interface.cyclo ./Core/Src/deca_probe_interface.d ./Core/Src/deca_probe_interface.o ./Core/Src/deca_probe_interface.su ./Core/Src/main.cyclo ./Core/Src/main.d ./Core/Src/main.o ./Core/Src/main.su ./Core/Src/stm32f1xx_hal_msp.cyclo ./Core/Src/stm32f1xx_hal_msp.d ./Core/Src/stm32f1xx_hal_msp.o ./Core/Src/stm32f1xx_hal_msp.su ./Core/Src/stm32f1xx_it.cyclo ./Core/Src/stm32f1xx_it.d ./Core/Src/stm32f1xx_it.o ./Core/Src/stm32f1xx_it.su ./Core/Src/syscalls.cyclo ./Core/Src/syscalls.d ./Core/Src/syscalls.o ./Core/Src/syscalls.su ./Core/Src/sysmem.cyclo ./Core/Src/sysmem.d ./Core/Src/sysmem.o ./Core/Src/sysmem.su ./Core/Src/system_stm32f1xx.cyclo ./Core/Src/system_stm32f1xx.d ./Core/Src/system_stm32f1xx.o ./Core/Src/system_stm32f1xx.su ./Core/Src/uwb_port.cyclo ./Core/Src/uwb_port.d ./Core/Src/uwb_port.o ./Core/Src/uwb_port.su

.PHONY: clean-Core-2f-Src

