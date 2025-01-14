/*** DOCUMENTATION
- https://www.kernel.org/doc/html/latest/userspace-api/media/drivers/uvcvideo.html
    info 1: uvc queries are explained
    info 2: units can be found by parsing the uvc descriptor
- https://www.mail-archive.com/search?l=linux-uvc-devel@lists.berlios.de&q=subject:%22Re%5C%3A+%5C%5BLinux%5C-uvc%5C-devel%5C%5D+UVC%22&o=newest&f=1
    info 1: selector is on 8 bits and since the manufacturer does not provide a driver, it is impossible to know which value it is.
***/

#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/uvcvideo.h>
#include <iostream>
#include <thread>
#include <vector>
using namespace std;

// opencv (used in is_emitter_working())
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wconversion"
#include <opencv2/videoio.hpp>
#include <opencv2/core/utils/logger.hpp>
#pragma GCC diagnostic pop

#include "query.h"
#include "driver.hpp"

#define EXIT_FD_ERROR 126

/**
 * @brief Print the control value in the standart output (without eol character)
 *
 * @param ctrl control value
 * @param len size of the control value
 */
inline void print_ctrl(const uint8_t *control, const uint16_t len)
{
    for (uint16_t i = 0; i < len; ++i)
        cout << " " << (int)control[i];
}

/**
 * @brief Print debug information in case of error during control reseting
 *
 * @param unit extension unit ID
 * @param selector control selector
 * @param control control value
 * @param ctrlSize size of the control
 */
inline void print_error_reset_debug(const uint8_t unit, const uint8_t selector, const uint8_t *control, const uint16_t ctrlSize)
{
    cerr << "ERROR: Impossible to reset the control." << endl;
    cout << "INFO: Please keep this debug in case of issue :" << endl;
    cout << "DEBUG: unit: " << (int)unit << ", selector: " << (int)selector << ", control:";
    print_ctrl(control, ctrlSize);
    cout << endl;
}

/**
 * @brief Print debug information about drivers to test
 *
 * @param unit extension unit ID
 * @param selector control selector
 * @param curCtrl current control value
 * @param nextCtrl first control value to test
 * @param resCtrl resolution control value
 * @param maxCtrl maximum control value
 * @param ctrlSize size of the control
 */
inline void print_driver_debug(const uint8_t unit, const uint8_t selector, const uint8_t *curCtrl, const uint8_t *nextCtrl, const uint8_t *resCtrl, const uint8_t *maxCtrl, const uint16_t ctrlSize)
{

    cout << "DEBUG: unit: " << (int)unit << ", selector: " << (int)selector;
    cout << ", cur control:";
    print_ctrl(curCtrl, ctrlSize);
    cout << ", first control to test:";
    print_ctrl(nextCtrl, ctrlSize);
    cout << ", res control:";
    print_ctrl(resCtrl, ctrlSize);
    cout << ", max control:";
    print_ctrl(maxCtrl, ctrlSize);
    cout << endl;
}

/**
 * @brief Execute shell command and return the ouput
 *
 * @param cmd command
 * @return output
 */
inline string *shell_exec(const string cmd)
{
    char buffer[128];
    string *result = new string();
    unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe)
    {
        *result = "error";
        return result;
    }

    while (fgets(buffer, 128 * sizeof(char), pipe.get()) != nullptr)
        *result += buffer;
    result->erase(result->end() - 1); // remove last \n
    return result;
}

/**
 * @brief Trigger the infrared emitter and ask the question:
 *        "Did you see the ir emitter flashing (not just turn on) ? Yes/No ? "
 *
 * @param deviceID id of the camera device
 *
 * @return 0 if the user inputed Yes, 1 if the user inputed No, 126 if unable to open the camera device
 */
inline int is_emitter_working(const int deviceID)
{
    cv::VideoCapture cap;
    cv::Mat frame;
    if (!cap.open(deviceID, cv::CAP_V4L2) || !cap.read(frame))
    {
        cerr << "CRITICAL: Cannot access to /dev/video" << deviceID << endl;
        return 126;
    }

    string answer;
    cout << "Is the ir emitter flashing (not just turn on) ? Yes/No ? ";
    cin >> answer;
    while (answer != "yes" && answer != "y" && answer != "Yes" && answer != "no" && answer != "n" && answer != "No")
    {
        cout << "Yes/No ? ";
        cin >> answer;
    }

    cap.release();
    frame.release();
    return answer == "no" || answer == "n" || answer == "No";
}

/**
 * @brief Get all the extension unit ID of the camera device
 *
 * @param device path to the camera /dev/videoX
 *
 * @return vector<uint8_t>* list of units
 */
inline const vector<uint8_t> *get_units(const char *device)
{
    const string *vid = shell_exec("udevadm info " + string(device) + " | grep -oP 'E: ID_VENDOR_ID=\\K.*'");
    const string *pid = shell_exec("udevadm info " + string(device) + " | grep -oP 'E: ID_MODEL_ID=\\K.*'");
    const string *units = shell_exec("lsusb -d" + *vid + ":" + *pid + " -v | grep bUnitID | grep -Eo '[0-9]+'");
    auto *unitsList = new vector<uint8_t>;

    unsigned i = 0, j = 0;
    for (; j < units->length(); ++j)
        if (units->at(j) == '\n')
        {
            unitsList->push_back((uint8_t)stoi(units->substr(i, j - i)));
            i = j + 1;
        }
    unitsList->push_back((uint8_t)stoi(units->substr(i, j - i)));

    delete vid;
    delete pid;
    delete units;
    return unitsList;
}

/**
 * @brief Compute the next possible control value
 *
 * @param curCtrl last executed control value
 * @param resCtrl resolution control value
 * @param maxCtrl maximum control value
 * @param ctrlSize len of the control value
 * @return 1 if there is no more possible control value, otherwise 0
 */
inline int get_next_curCtrl(uint8_t *curCtrl, const uint8_t *resCtrl, const uint8_t *maxCtrl, const uint16_t ctrlSize)
{
    if (!memcmp(curCtrl, maxCtrl, ctrlSize * sizeof(uint8_t))) // curCtrl == maxCtrl
        return 1;

    for (unsigned i = 0; i < ctrlSize; ++i)
    {
        int nextCtrl = curCtrl[i] + resCtrl[i]; // int to avoid overflow
        curCtrl[i] = (uint8_t)nextCtrl;
        if (nextCtrl > maxCtrl[i]) // resCtrl does not allow to reach maxCtrl
        {
            memcpy(curCtrl, maxCtrl, ctrlSize * sizeof(uint8_t)); // set maxCtrl
            return 0;
        }
    }
    return 0;
}

/**
 * @brief Open a file descriptor
 *
 * @param device device to open a fd
 * @return fd or -1 if unable to open
 */
inline int open_fd(const char *device)
{
    errno = 0;
    int fd = open(device, O_WRONLY);
    if (fd < 0 || errno)
    {
        cerr << "CRITICAL: Cannot access to " << device << endl;
        return -1;
    }

    return fd;
}

/**
 * Generate a driver for the infrared emitter
 *
 * usage: driver-generator [device] [negAnswerLimit] [driverFile] [debug]
 *        device           path to the infrared camera, it must be of the form /dev/videoX
 *        negAnswerLimit   after k negative answer the pattern will be skiped. Use 256 for unlimited
 *        driverFile       path where the driver will be written
 *        debug            1 for print debug information, otherwise 0
 *
 * See std output for debug information and stderr for error information
 *
 * Exit code: 0 Success
 *            1 Error
 *            126 Unable to open the camera device
 */
int main(int, const char *argv[])
{
    cv::utils::logging::setLogLevel(cv::utils::logging::LogLevel::LOG_LEVEL_ERROR);

    const char *device = argv[1];
    int result;
    sscanf(device, "/dev/video%d", &result);
    const int deviceID = result;
    const int negAnswerLimit = atoi(argv[2]);
    const char *driverFile = argv[3];
    const bool debug = atoi(argv[4]);

    result = is_emitter_working(deviceID);
    if (!result)
    {
        cerr << "ERROR: Your emiter is already working, skipping the configuration." << endl;
        return EXIT_FAILURE;
    }
    else if (result == 126)
        return EXIT_FD_ERROR;

    int fd = open_fd(device);
    if (fd < 0)
        return EXIT_FD_ERROR;

    // begin research
    const vector<uint8_t> *units = get_units(device);
    for (const uint8_t unit : *units)
    {
        for (int __selector = 0; __selector < 256; ++__selector)
        {
            const uint8_t selector = __selector; // safe: 0 <= __selector <= 255

            // get the control instruction lenght
            const uint16_t ctrlSize = len_uvc_query(fd, unit, selector);
            if (!ctrlSize)
                continue;

            // get the current control value
            uint8_t curCtrl[ctrlSize] = {};
            if (get_uvc_query(UVC_GET_CUR, fd, unit, selector, ctrlSize, curCtrl))
                continue;

            // check if the control value can be modified
            if (set_uvc_query(fd, unit, selector, ctrlSize, curCtrl))
                continue;

            // try to get the maximum control value (the value does not necessary exists)
            uint8_t maxCtrl[ctrlSize] = {};
            if (get_uvc_query(UVC_GET_MAX, fd, unit, selector, ctrlSize, maxCtrl))
                memset(maxCtrl, 255, ctrlSize * sizeof(uint8_t)); // use the 255 array

            // try get the resolution control value (the value does not necessary exists)
            uint8_t resCtrl[ctrlSize] = {};
            if (get_uvc_query(UVC_GET_RES, fd, unit, selector, ctrlSize, resCtrl))
            {
                if (debug)
                    cout << "DEBUG: Computing the resolution control." << endl;
                for (unsigned i = 0; i < ctrlSize; ++i)
                    resCtrl[i] = curCtrl[i] != maxCtrl[i]; // step of 0 or 1
            }

            // try get the minimum control value (the value does not necessary exists) to start from it
            uint8_t nextCtrl[ctrlSize] = {};
            result = get_uvc_query(UVC_GET_MIN, fd, unit, selector, ctrlSize, nextCtrl);
            if (result || !memcmp(curCtrl, nextCtrl, ctrlSize * sizeof(uint8_t))) // or: the min control value is the current control
            {
                memcpy(nextCtrl, curCtrl, ctrlSize * sizeof(uint8_t));      // set the current value
                if (get_next_curCtrl(nextCtrl, resCtrl, maxCtrl, ctrlSize)) // and start from the next one
                    continue;                                               // the current control value is equal to the maximal control
            }

            if (debug)
                print_driver_debug(unit, selector, curCtrl, nextCtrl, resCtrl, maxCtrl, ctrlSize);

            // try to find the right control value
            int negAnswerCounter = 0;
            result = 0;
            while (!result && negAnswerCounter < negAnswerLimit)
            {
                result = set_uvc_query(fd, unit, selector, ctrlSize, nextCtrl);
                if (!result)
                {
                    close(fd);
                    result = is_emitter_working(deviceID);
                    if (!result) // found
                    {
                        delete units;
                        return write_driver(driverFile, device, unit, selector, ctrlSize, nextCtrl);
                    }
                    else if (result == 126) // if unable to test the camera, reset the control and exit
                    {
                        if (set_uvc_query(fd, unit, selector, ctrlSize, curCtrl))
                            print_error_reset_debug(unit, selector, curCtrl, ctrlSize);
                        delete units;
                        return EXIT_FD_ERROR;
                    }
                    fd = open_fd(device);
                    if (fd < 0)
                    {
                        delete units;
                        return EXIT_FD_ERROR;
                    }
                }

                ++negAnswerCounter;
                result = get_next_curCtrl(nextCtrl, resCtrl, maxCtrl, ctrlSize);
            }

            if (debug && negAnswerCounter >= negAnswerLimit)
                cout << "DEBUG: Negative answer limit exceeded, skipping the pattern." << endl;

            // reset the control
            if (set_uvc_query(fd, unit, selector, ctrlSize, curCtrl))
                print_error_reset_debug(unit, selector, curCtrl, ctrlSize);
        }
    }

    close(fd);
    delete units;
    return EXIT_FAILURE;
}
