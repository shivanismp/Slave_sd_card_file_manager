#ifndef __MACHINE_TYPE_SELECT_H__
#define __MACHINE_TYPE_SELECT_H__

// Machine type selection
#define _MACHINE_TEMPERATURE_CNTRL_
// #define _MACHINE_TIMER_CNTRL_


/* MACHINE-SPECIFIC FEEDBACK: fixed addresses, selected meaning. */
#if defined(_MACHINE_TEMPERATURE_CNTRL_) && defined(_MACHINE_TIMER_CNTRL_)
    #error "Select only one machine control mode in machine_type_select.h"
#elif !defined(_MACHINE_TEMPERATURE_CNTRL_) && !defined(_MACHINE_TIMER_CNTRL_)
    #error "Select a machine control mode in machine_type_select.h"
#endif

#endif