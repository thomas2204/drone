################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Core/Inc/dwt_uwb_driver/dw3000/dw3000_device.c 

OBJS += \
./Core/Inc/dwt_uwb_driver/dw3000/dw3000_device.o 

C_DEPS += \
./Core/Inc/dwt_uwb_driver/dw3000/dw3000_device.d 


# Each subdirectory must supply rules for building sources it contributes
Core/Inc/dwt_uwb_driver/dw3000/%.o Core/Inc/dwt_uwb_driver/dw3000/%.su Core/Inc/dwt_uwb_driver/dw3000/%.cyclo: ../Core/Inc/dwt_uwb_driver/dw3000/%.c Core/Inc/dwt_uwb_driver/dw3000/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m3 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F103xE -c -I../Core/Inc -I"C:/Users/Matthew/STM32CubeIDE/workspace_2.2.0/Beacon/Core/Inc/dwt_uwb_driver/lib/qmath/include" -I"C:/Users/Matthew/STM32CubeIDE/workspace_2.2.0/Beacon/Core/Inc/dwt_uwb_driver" -I"C:/Users/Matthew/STM32CubeIDE/workspace_2.2.0/Beacon/Core/Inc/dwt_uwb_driver/dw3000" -I"C:/Users/Matthew/STM32CubeIDE/workspace_2.2.0/Beacon/Core/Inc/dwt_uwb_driver/lib/qmath" -I"C:/Users/Matthew/STM32CubeIDE/workspace_2.2.0/Beacon/Core/Inc/dwt_uwb_driver/lib/qmath" -I"C:/Users/Matthew/STM32CubeIDE/workspace_2.2.0/Beacon/Core/Inc/dwt_uwb_driver/dw3720" -I../Drivers/STM32F1xx_HAL_Driver/Inc -I../Drivers/STM32F1xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F1xx/Include -I../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfloat-abi=soft -mthumb -o "$@"

clean: clean-Core-2f-Inc-2f-dwt_uwb_driver-2f-dw3000

clean-Core-2f-Inc-2f-dwt_uwb_driver-2f-dw3000:
	-$(RM) ./Core/Inc/dwt_uwb_driver/dw3000/dw3000_device.cyclo ./Core/Inc/dwt_uwb_driver/dw3000/dw3000_device.d ./Core/Inc/dwt_uwb_driver/dw3000/dw3000_device.o ./Core/Inc/dwt_uwb_driver/dw3000/dw3000_device.su

.PHONY: clean-Core-2f-Inc-2f-dwt_uwb_driver-2f-dw3000

