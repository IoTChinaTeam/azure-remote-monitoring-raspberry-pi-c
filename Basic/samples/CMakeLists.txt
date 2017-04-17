#Copyright (c) Microsoft. All rights reserved.
#Licensed under the MIT license. See LICENSE file in the project root for full license information.

#this is CMakeLists.txt for serializer samples. There's nothing here, except redirections to 
#individual samples

set(PLATFORM_INC_FOLDER ${CMAKE_CURRENT_LIST_DIR}/platform_specific/inc CACHE INTERNAL "this is what needs to be included if using bme280 sensor and locked file lib" FORCE)

include_directories(${SERIALIZER_INC_FOLDER} ${SHARED_UTIL_INC_FOLDER} ${PLATFORM_INC_FOLDER})

function(add_sample_directory whatIsBuilding)
  add_subdirectory(${whatIsBuilding})

  set_target_properties(${whatIsBuilding}
                     PROPERTIES
                     FOLDER "Serializer_Samples")
endfunction()

add_subdirectory(platform_specific)

if(${use_amqp_kit})
  add_sample_directory(remote_monitoring)
  add_sample_directory(simplesample_amqp)
endif()

if(${use_http_kit})
  message(SEND_ERROR "The kit for Raspberrypi do not support http yet")
endif()

if(${use_mqtt_kit})
  message(SEND_ERROR "The kit for Raspberrypi do not support mqtt yet")
endif()

