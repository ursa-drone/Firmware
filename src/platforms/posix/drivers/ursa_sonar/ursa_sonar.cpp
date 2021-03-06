/****************************************************************************
 *
 * Copyright (c) 2017 LAOSAAC
 *
 ****************************************************************************/

/**
 * @file ursa_sonar.cpp
 * Lightweight driver to read sonar measurements via DMA timed GPIO
 */

#include <px4_config.h>
#include <px4_workqueue.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <px4_getopt.h>
#include <errno.h>

#include <systemlib/perf_counter.h>
#include <systemlib/err.h>

#include <drivers/drv_hrt.h>
#include <uORB/uORB.h>
#include <uORB/topics/distance_sensor.h>

#include <board_config.h>

#include <ursa_gpio/ursa_gpio.hpp>
#include <DevMgr.hpp>


extern "C" { __EXPORT int ursa_sonar_main(int argc, char *argv[]); }

using namespace DriverFramework;

#define MAX_STR 35
#define URSA_SONAR_PULSE_INTERVAL_US 100000
#define MAX_SONAR_RANGE 4
#define MIN_SONAR_RANGE 0.02

class UrsaSonarPub
{
public:
    UrsaSonarPub();
    ~UrsaSonarPub();


    /**
     * Setup callbacks etc
     *
     * @return 0 on success
     */
    int init(int gpio_Out, int gpio_In);

    /**
     * Stop automatic measurement.
     *
     * @return 0 on success
     */
    int stop();

    void process_pulse(int gpio, int val, uint32_t tick);
    gpio_write_t callbackStruct;
    gpio_write_t pulseStruct;

    /* Trampoline for the work queue. */
    static void cycle_trampoline(void *arg);

private:
    DevHandle _gpio_handle;
    uint32_t _startpulse;
    void *_gpio_map;
    int _publish();
    void _cycle();
    struct distance_sensor_s _sonar_data;
    orb_advert_t _sonar_topic;
    int _gpio_Out;
    int _gpio_In;
    bool _shouldExit;
    struct work_s _work;
    double _range;
};

UrsaSonarPub::UrsaSonarPub() :
    _startpulse(0),
    _sonar_topic(nullptr),
    _shouldExit(false),
    _range(0.0f)
{
}

UrsaSonarPub::~UrsaSonarPub()
{
}

int UrsaSonarPub::init(int gpio_Out, int gpio_In)
{
    _gpio_In=gpio_In;
    _gpio_Out=gpio_Out;
    _sonar_data.min_distance = MIN_SONAR_RANGE;
    _sonar_data.max_distance = MAX_SONAR_RANGE;
    _sonar_data.type = distance_sensor_s::MAV_DISTANCE_SENSOR_ULTRASOUND;
    _sonar_data.id = 0;
    //_sonardata.orientation=MAV_SENSOR_ROTATION_NONE;

    //Get a handle to our timed GPIO DMA magic
    DevMgr::getHandle(GPIO_DEV_PATH, _gpio_handle); 

    if (!_gpio_handle.isValid()) {
        PX4_ERR("Failed to get handle to timed GPIO device");
        return -1;
    }

    // Setup the callback struct which we'll pass to the timed GPIO device
    callbackStruct.callback=std::bind(&UrsaSonarPub::process_pulse, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
    callbackStruct.gpio=gpio_In;
    callbackStruct.type=GPIO_CALLBACK;
    _gpio_handle.write((void*)&callbackStruct,sizeof(gpio_write_t));

    // Setup GPIO write struct on output and write low
    pulseStruct.gpio=gpio_Out;
    pulseStruct.type=GPIO_WRITE;
    pulseStruct.value=0;
    _gpio_handle.write((void*)&pulseStruct,sizeof(gpio_write_t));

    _cycle();

    return 0;
}

void UrsaSonarPub::cycle_trampoline(void *arg)
{
    UrsaSonarPub *dev = reinterpret_cast<UrsaSonarPub *>(arg);
    dev->_cycle();
}

void UrsaSonarPub::_cycle()
{
    // Send out a pulse on the GPIO
    pulseStruct.value=1;
    _gpio_handle.write((void*)&pulseStruct,sizeof(gpio_write_t));
    usleep(10);
    pulseStruct.value=0;
    _gpio_handle.write((void*)&pulseStruct,sizeof(gpio_write_t));

    if (!_shouldExit) {
        work_queue(HPWORK, &_work, (worker_t)&UrsaSonarPub::cycle_trampoline, this,
               USEC2TICK(URSA_SONAR_PULSE_INTERVAL_US));
    }
}

int UrsaSonarPub::stop()
{
    /* Stop sensor. */
    // TODO add interface to release callbacks from GPIO

    PX4_INFO("Stopping RC Publisher...");

    //Un-export output pin
    int gpio_fd;
    char buf[3];
    if ((gpio_fd = open("/sys/class/gpio/unexport", O_RDWR | O_SYNC)) < 0) {
        PX4_ERR("Failed to open pin unexport file descriptor");
        return -1;
    }
    snprintf(buf, 3*sizeof(char), "%d", _gpio_Out);
    write(gpio_fd, buf, 3);
    close(gpio_fd);

    _shouldExit=true;

    return 0;
}

void UrsaSonarPub::process_pulse(int gpio, int val, uint32_t tick){
    if (val==1){
        _startpulse=tick;
        return;
    }
    uint32_t time = tick-_startpulse;
    double range = time/5882.35;
    if (range>MIN_SONAR_RANGE && range<MAX_SONAR_RANGE){
        _range=range;
        _publish();
    }
    return;
}

int UrsaSonarPub::_publish(){
    uint64_t ts = hrt_absolute_time();
    _sonar_data.timestamp = ts;
    _sonar_data.current_distance=_range;
    _sonar_data.covariance=0.0f;

    if (_sonar_topic == nullptr) {
        _sonar_topic = orb_advertise(ORB_ID(distance_sensor), &_sonar_data);

    } else {
        orb_publish(ORB_ID(distance_sensor), _sonar_topic, &_sonar_data);
    }
    return 0;
}


namespace ursa_sonar
{
// Singleton since we only want/need one right now
UrsaSonarPub *g_dev = nullptr;

int start(int gpio_Out, int gpio_In);
int stop();
int info();
void usage();

int start(int gpio_Out, int gpio_In)
{
    if (g_dev != nullptr) {
        PX4_ERR("Ursa sonar already running!");
        return -1;
    }

    g_dev = new UrsaSonarPub();

    if (g_dev == nullptr) {
        PX4_ERR("failed instantiating UrsaSonarPub object");
        return -1;
    }

    int ret = g_dev->init(gpio_Out, gpio_In);

    if (ret != 0) {
        PX4_ERR("UrsaSonarPub init failed");
        return ret;
    }
    PX4_INFO("Started ursa_sonar, output pin: %d, input pin: %d",gpio_Out,gpio_In);

    return 0;
}

int stop()
{
    if (g_dev == nullptr) {
        PX4_INFO("UrsaSonarPub tried to stop but not running");
        return 1;
    }

    int ret = g_dev->stop();

    if (ret != 0) {
        PX4_ERR("UrsaSonarPub could not be stopped");
        return ret;
    }

    delete g_dev;
    g_dev = nullptr;
    return 0;
}

/**
 * Print a little info about the driver.
 */
int info()
{
    if (g_dev == nullptr) {
        PX4_ERR("driver not running");
        return 1;
    }

    PX4_DEBUG("state @ %p", g_dev);

    return 0;
}

void usage()
{
    PX4_WARN("Usage: ursa_sonar 'start [gpio out] [gpio in]', 'info', 'stop'");
}

} // namespace ursa_sonar


int ursa_sonar_main(int argc, char *argv[])
{
    int ret = 0;

    if (argc <= 1) {
        ursa_sonar::usage();
        return 1;
    }

    const char *verb = argv[1];

    if (!strcmp(verb, "start")) {
        if (argc <= 3) {
            ursa_sonar::usage();
            return 1;
        }
        ret = ursa_sonar::start(atoi(argv[2]),atoi(argv[3]));
    }

    else if (!strcmp(verb, "stop")) {
        ret = ursa_sonar::stop();
    }

    else if (!strcmp(verb, "info")) {
        ret = ursa_sonar::info();
    }

    else {
        ursa_sonar::usage();
        return 1;
    }

    return ret;
}
