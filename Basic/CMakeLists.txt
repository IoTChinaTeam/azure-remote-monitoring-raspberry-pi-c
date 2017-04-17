#Copyright (c) Microsoft. All rights reserved.
#Licensed under the MIT license. See LICENSE file in the project root for full license information.

cmake_minimum_required(VERSION 2.8.11)

project(iot-hub-c-raspberrypi-getstartedkit)

option(use_amqp_kit "use samples provided in the kit" ON)

add_subdirectory(azure-iot-sdk-c)

add_subdirectory(samples)
