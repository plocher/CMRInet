/*
 * Services.h — SimpleHost shim over shared HostServices overlay.
 */
#ifndef SERVICES_H
#define SERVICES_H

#include "HostServices.h"

using Service = CMRInet::app::Service;
using Orchestrator = CMRInet::app::Orchestrator;
using BitWalkerConfig = CMRInet::app::BitWalkerConfig;
using BitWalkerService = CMRInet::app::BitWalkerService;
using InputToggleConfig = CMRInet::app::InputToggleConfig;
using InputToggleService = CMRInet::app::InputToggleService;

// Global orchestrator for this sketch
extern Orchestrator g_orchestrator;

#endif
