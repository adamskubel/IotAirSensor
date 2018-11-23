#include <stdio.h>

// #include "esp8266/gpio_register.h"
#include "eagle_soc.h"

#include "common/cs_dbg.h"
#include "common/platform.h"
#include "frozen/frozen.h"
#include "mgos_app.h"
#include "mgos_adc.h"
#include "mgos_gpio.h"
#include "mgos_hal.h"
#include "mgos_timers.h"
#include "mgos_aws_shadow.h"

#define STATE_FMT "{fan: %d, temperature: %d}"

#define NUMSAMPLES 5

int D_PINS[] = {4,5};
int INPUT_PIN = 14;
int POWER_PIN = 4;
int SPEED_PIN = 5;
int PIN_COUNT = 2;
bool init = false;
int pressLength = 500;
int buttonDelay = 1000;
int SPEED_COUNT = 4;


#define GPIO_PIN_ADDR(i)        (GPIO_PIN0_ADDRESS + i*4)

static int readTemp(void) {
  int i;
  int samples[NUMSAMPLES];
  int average;
 
  // take N samples in a row, with a slight delay
  for (i=0; i< NUMSAMPLES; i++) {
   samples[i] = mgos_adc_read(0);
    LOG(LL_INFO, ("Sample[%d]=%d", i, samples[i]));
   mgos_msleep(10);
  }
 
  average = 0;
  for (i=0; i< NUMSAMPLES; i++) {
     average += samples[i];
  }
  return average;
}

static void report_state(void * arg) {
  if (arg != 0)
    LOG(LL_INFO, ("fuck%d", sizeof(arg)));

  char buf[100];
  struct json_out out = JSON_OUT_BUF(buf, sizeof(buf));
  int temp = readTemp();
  json_printf(&out, STATE_FMT, temp);
  LOG(LL_INFO, ("== Reporting state: %s", buf));
  mgos_aws_shadow_updatef(0, "{reported:" STATE_FMT "}", -1, temp);
}

static void press(int pin) {
  LOG(LL_INFO,("Pressing pin %d", pin));
  mgos_gpio_write(pin, 0);
  mgos_msleep(pressLength);
  mgos_gpio_write(pin, 1);
  mgos_msleep(buttonDelay);
}

static bool readLed() {
  bool val = !mgos_gpio_read(INPUT_PIN);
  LOG(LL_INFO, ("LED1 has value %d", val));
  return val;
}

static void setToLedState() {
  int count = SPEED_COUNT;
  while (!readLed() && count-- > 0) {    
    press(SPEED_PIN);    
  }
  if (!readLed()) {
    press(POWER_PIN);
  }
  count = SPEED_COUNT;
  while (!readLed() && count-- > 0) {    
    press(SPEED_PIN);    
  }
  if (!readLed()) {
    LOG(LL_INFO, ("Welp, can't figure this one out!"));
  }
}

static void setFanState(int fanState) {
  if (!init) {
    LOG(LL_ERROR, ("y u no init?"));
    return;
  }
  LOG(LL_INFO, ("Setting fan state to %d", fanState));

  if (fanState > 0) {
    setToLedState();
    if (fanState == 1) 
      return;
    int steps = (5 - fanState);
    for (int i=0;i < steps; i++)  {
      press(SPEED_PIN);
    }
  }
  else {
    int count = SPEED_COUNT;
    while (!readLed() && count-- > 0) {    
      press(SPEED_PIN);    
    }    
    if (readLed()) {
      LOG(LL_INFO, ("Currently on, turning off"));
      press(POWER_PIN);
    } else {
      LOG(LL_INFO, ("Fan already off"));
    }
  }
}

static void aws_shadow_state_handler(void *arg, enum mgos_aws_shadow_event ev,
                                     uint64_t version,
                                     const struct mg_str reported,
                                     const struct mg_str desired,
                                     const struct mg_str reported_md,
                                     const struct mg_str desired_md) {
  LOG(LL_INFO, ("== Event: %d (%s), version: %llu", ev,
                mgos_aws_shadow_event_name(ev), version));
  
  // if (ev != MGOS_AWS_SHADOW_CONNECTED &&
    if (ev != MGOS_AWS_SHADOW_GET_ACCEPTED &&
      ev != MGOS_AWS_SHADOW_UPDATE_DELTA) {
    LOG(LL_INFO, ("Event type %d will be ignored", ev));
    return;
  }

  LOG(LL_INFO, ("Reported state: %.*s", (int) reported.len, reported.p));
  LOG(LL_INFO, ("Desired state : %.*s", (int) desired.len, desired.p));
  LOG(LL_INFO,
      ("Reported metadata: %.*s", (int) reported_md.len, reported_md.p));
  LOG(LL_INFO, ("Desired metadata : %.*s", (int) desired_md.len, desired_md.p));

  int fan, newButtonDelay, newPressLength;
  
  int count = json_scanf(desired.p, desired.len, "{fan:%d}", &fan);
  if (!count) {
    LOG(LL_INFO, ("Failed to parse shadow. Parse count = %d", count));
    return;
  }
  
  count = json_scanf(desired.p, desired.len, "{fan-config:{press-length:%d, button-delay:%d}}", &newPressLength, &newButtonDelay);
  
  if (count == 2) {
    if (newPressLength > 10 && newPressLength < 10000) {
      pressLength = newPressLength;
    }

    if (newButtonDelay > 10 && newButtonDelay < 10000) {
      buttonDelay = newButtonDelay;
    }

    LOG(LL_INFO, ("Config settings: pressLength=%d, buttonDelay=%d", pressLength, buttonDelay));
  }

  setFanState(fan);

  (void) arg;
}

enum mgos_app_init_result mgos_app_init(void) {
  LOG(LL_INFO,("Initializing GPIO%d",0));  
  init = false;   
  mgos_gpio_init();
  for (int i=0; i<PIN_COUNT; i++) {    
    int pin = D_PINS[i];
    mgos_gpio_set_mode(pin, MGOS_GPIO_MODE_OUTPUT);
    GPIO_REG_WRITE(GPIO_PIN_ADDR(GPIO_ID_PIN(pin)), GPIO_PIN_PAD_DRIVER_SET(GPIO_PAD_DRIVER_ENABLE));  
    if (!mgos_gpio_set_pull(pin, MGOS_GPIO_PULL_NONE)) {
      LOG(LL_ERROR, ("Failed to pull"));
      return MGOS_INIT_APP_INIT_FAILED;
    }
    mgos_gpio_write(pin, 1); 
  }
  if (!mgos_gpio_set_pull(INPUT_PIN, MGOS_GPIO_PULL_NONE)) {
     LOG(LL_ERROR, ("Failed to pull"));
     return MGOS_INIT_APP_INIT_FAILED;
  }
  mgos_gpio_set_mode(INPUT_PIN, MGOS_GPIO_MODE_INPUT);

  mgos_adc_enable(0);
  mgos_aws_shadow_set_state_handler(aws_shadow_state_handler, NULL);

  // mgos_set_timer(20000, 1, report_state, NULL);
  init = true;
  return MGOS_APP_INIT_SUCCESS;
}
