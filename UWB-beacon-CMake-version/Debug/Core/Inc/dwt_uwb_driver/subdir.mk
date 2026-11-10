################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Core/Inc/dwt_uwb_driver/deca_compat.c \
../Core/Inc/dwt_uwb_driver/deca_interface.c \
../Core/Inc/dwt_uwb_driver/deca_rsl.c 

OBJS += \
./Core/Inc/dwt_uwb_driver/deca_compat.o \
./Core/Inc/dwt_uwb_driver/deca_interface.o \
./Core/Inc/dwt_uwb_driver/deca_rsl.o 

C_DEPS += \
./Core/Inc/dwt_uwb_driver/deca_compat.d \
./Core/Inc/dwt_uwb_driver/deca_interface.d \
./Core/Inc/dwt_uwb_driver/deca_rsl.d 


# Each subdirectory must supply rules for building sources it contributes
Core/Inc/dwt_uwb_driver/%.o Core/Inc/dwt_uwb_driver/%.su Core/Inc/dwt_uwb_driver/%.cyclo: ../Core/Inc/dwt_uwb_driver/%.c Core/Inc/dwt_uwb_driver/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m3 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F103xE -c -I../Core/Inc -I"C:/Users/Matthew/STM32CubeIDE/workspace_2.2.0/Beacon/Core/Inc/dwt_uwb_driver/lib/qmath/include" -I"C:/Users/Matthew/STM32CubeIDE/workspace_2.2.0/Beacon/Core/Inc/dwt_uwb_driver" -I"C:/Users/Matthew/STM32CubeIDE/workspace_2.2.0/Beacon/Core/Inc/dwt_uwb_driver/dw3000" -I"C:/Users/Matthew/STM32CubeIDE/workspace_2.2.0/Beacon/Core/Inc/dwt_uwb_driver/lib/qmath" -I"C:/Users/Matthew/STM32CubeIDE/workspace_2.2.0/Beacon/Core/Inc/dwt_uwb_driver/lib/qmath" -I"C:/Users/Matthew/STM32CubeIDE/workspace_2.2.0/Beacon/Core/Inc/dwt_uwb_driver/dw3720" -I../Drivers/STM32F1xx_HAL_Driver/Inc -I../Drivers/STM32F1xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F1xx/Include -I../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfloat-abi=soft -mthumb -o "$@"

clean: clean-Core-2f-Inc-2f-dwt_uwb_driver

clean-Core-2f-Inc-2f-dwt_uwb_driver:
	-$(RM) ./Core/Inc/dwt_uwb_driver/deca_compat.cyclo ./Core/Inc/dwt_uwb_driver/deca_compat.d ./Core/Inc/dwt_uwb_driver/deca_compat.o ./Core/Inc/dwt_uwb_driver/deca_compat.su ./Core/Inc/dwt_uwb_driver/deca_interface.cyclo ./Core/Inc/dwt_uwb_driver/deca_interface.d ./Core/Inc/dwt_uwb_driver/deca_interface.o ./Core/Inc/dwt_uwb_driver/deca_interface.su ./Core/Inc/dwt_uwb_driver/deca_rsl.cyclo ./Core/Inc/dwt_uwb_driver/deca_rsl.d ./Core/Inc/dwt_uwb_driver/deca_rsl.o ./Core/Inc/dwt_uwb_driver/deca_rsl.su

.PHONY: clean-Core-2f-Inc-2f-dwt_uwb_driver

