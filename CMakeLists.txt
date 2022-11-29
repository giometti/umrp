# Copyright (c) 2020 Microchip Technology Inc. and its subsidiaries.
# SPDX-License-Identifier: (GPL-2.0)

project (mrp C)
set(MRP_IFDRIVER netlink CACHE STRING "networking hardware driver")
set_property(CACHE MRP_IFDRIVER PROPERTY STRINGS netlink kbact)
option(MRP_HAVE_DBus1 "DBus RPC support" OFF)

cmake_minimum_required(VERSION 2.6)

## libnl ################################################
find_path(LibNL_INCLUDE_DIR netlink/netlink.h
    /usr/include
    /usr/include/libnl3
    /usr/local/include
    /usr/local/include/libnl3
)

find_library(LibNL_LIBRARY NAMES nl nl-3)
find_library(LibNL_GENL_LIBRARY NAMES nl-genl nl-genl-3)

if (NOT (LibNL_INCLUDE_DIR AND LibNL_LIBRARY))
    message(FATAL_ERROR "Could not find netlink library.")
endif ()

## libmnl ###############################################
find_path(LibMNL_INCLUDE_DIR libmnl/libmnl.h
    /usr/include
    /usr/local/include
)

find_library(LibMNL_LIBRARY NAMES mnl)

if (NOT (LibMNL_INCLUDE_DIR AND LibMNL_LIBRARY))
    message(FATAL_ERROR "Coult not find mnl library.")
endif ()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DHAVE_LIBMNL")

## libev ################################################
find_path(LibEV_INCLUDE_DIR ev.h
    /usr/include
    /usr/local/include
)

find_library(LibEV_LIBRARY NAMES ev)

if (NOT (LibEV_INCLUDE_DIR AND LibEV_LIBRARY))
    message(FATAL_ERROR "Could not find libev.")
endif ()

## libcfm_netlink ########################################
find_path(LibCFM_INCLUDE_DIR cfm_netlink.h
    /usr/include
    /usr/local/include
)

find_library(LibCFM_LIBRARY NAMES cfm_netlink)
if (NOT (LibCFM_INCLUDE_DIR AND LibCFM_LIBRARY))
    message(FATAL_ERROR "Could not find libcfm_netlink.")
endif ()

## libdbus ########################################
if (MRP_HAVE_DBus1)
    find_package(DBus1 REQUIRED)

    set(MRP_DBUS_PATH "/org/mrp/Event" CACHE STRING "the path to the object emitting the signal")
    set(MRP_DBUS_IFACE "org.mrp.Event" CACHE STRING "the interface the signal is emitted from")

    set(MRP_SERVER_DBus1_CFLAGS "-DMRP_HAVE_DBUS
				-DMRP_DBUS_PATH=\\\"${MRP_DBUS_PATH}\\\"
				-DMRP_DBUS_IFACE=\\\"${MRP_DBUS_IFACE}\\\"")
    set(MRP_SERVER_DBus1_SRCS dbus.c)
endif ()

## mrp (this project) ####################################
execute_process (
    COMMAND git -C ${CMAKE_SOURCE_DIR} describe --tags --abbrev=10 --dirty --long --always
    OUTPUT_VARIABLE __VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

add_definitions(-Wall ${MRP_SERVER_DBus1_CFLAGS})

include_directories(${LibNL_INCLUDE_DIR} ${LibEV_INCLUDE_DIR} ${LibMNL_INCLUDE_DIR} ${LibCFM_INCLUDE_DIR} ${DBus1_INCLUDE_DIR} ${DBus1_ARCH_INCLUDE_DIR} include/uapi)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_GNU_SOURCE")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D__VERSION=\\\"${__VERSION}\\\"")

add_executable(mrp mrp.c)

set (MRP_IFDRIVER_SRC "ifdriver_${MRP_IFDRIVER}.c")
if (NOT EXISTS "${CMAKE_SOURCE_DIR}/${MRP_IFDRIVER_SRC}")
    message(FATAL_ERROR "no ${MRP_IFDRIVER_SRC} file! Unknown driver ${MRP_IFDRIVER}.")
endif ()

add_executable(mrp_server mrp_server.c packet.c server_socket.c server_cmds.c state_machine.c timer.c libnetlink.c utils.c ${MRP_SERVER_DBus1_SRCS} ${MRP_IFDRIVER_SRC})
target_link_libraries(mrp_server ${LibNL_LIBRARY} ${LibNL_GENL_LIBRARY}
    ${LibEV_LIBRARY} ${LibMNL_LIBRARY} ${LibCFM_LIBRARY} ${DBus1_LIBRARY})

install(TARGETS mrp_server mrp RUNTIME DESTINATION bin)

