#include <unistd.h>
#include <sstream>
#include <execinfo.h>

#include "daemon/beanstalk.hpp"
#include "video/logging_videobuffer.h"
#include "daemon/daemonconfig.h"
#include "inc/safequeue.h"

#include "tclap/CmdLine.h"
#include "alpr.h"
#include "openalpr/cjson.h"
#include "support/tinythread.h"
#include <curl/curl.h>
#include "support/timing.h"

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <log4cplus/configurator.h>
#include <log4cplus/consoleappender.h>
#include <log4cplus/fileappender.h>

#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string>
#include <unordered_map>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <regex>

#define PORT 8080

using namespace alpr;

// Variables
SafeQueue<cv::Mat> framesQueue;

// Prototypes
void streamRecognitionThread(void *arg);

// Constants
const std::string ALPRD_CONFIG_FILE_NAME = "alprd.conf";
const std::string OPENALPR_CONFIG_FILE_NAME = "openalpr.conf";
const std::string DEFAULT_LOG_FILE_PATH = "/var/log/alprd.log";

// Variable
std::string message = "START";
std::unordered_map<std::string, int> tableau;

struct CaptureThreadData {
    std::string company_id;
    std::string stream_url;
    std::string site_id;
    int camera_id;
    int analysis_threads;

    bool clock_on;

    std::string config_file;
    std::string country_code;
    std::string pattern;
    bool output_images;
    std::string output_image_folder;
    int top_n;
};

struct UploadThreadData {
    std::string upload_url;
};

void segfault_handler(int sig) {
    void *array[10];
    size_t size;

    // get void*'s for all entries on the stack
    size = backtrace(array, 10);

    // print out all the frames to stderr
    backtrace_symbols_fd(array, size, STDERR_FILENO);
    exit(1);
}

bool daemon_active;

static log4cplus::Logger logger;

int server_fd = -1;
int new_socket = -1;

int number_of_plates_total = 0;
int number_of_plates_in_table = 0;

void init_server_socket() {
    struct sockaddr_in address;
    int opt = 1;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *) &address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
}

void send_message_to_socket(const char *message) {
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    if (server_fd == -1) {
        init_server_socket();
    }

    if (new_socket == -1) {
        if ((new_socket = accept(server_fd, (struct sockaddr *) &address, (socklen_t * ) & addrlen)) < 0) {
            perror("accept");
            exit(EXIT_FAILURE);
        }
    }

    std::string input = message;
    std::string transformed_message = input.substr(0, 2) + "-" + input.substr(2, 3) + "-" + input.substr(5, 2);

    ssize_t sent = send(new_socket, transformed_message.c_str(), transformed_message.length(), 0);
    if (sent == -1) {
        perror("send failed");
    }
}

void send_message_to_serial_port(const char *message) {
    int port = open("/dev/ttyAMA0", O_RDWR | O_NOCTTY);
    LOG4CPLUS_INFO(logger, " ------------> Message to send : " << message);

    if (port == -1) {
        LOG4CPLUS_ERROR(logger, "Error opening serial port.");
    }

    const char *baud_rate_command = "stty -F /dev/ttyAMA0 9600";
    system(baud_rate_command);

    std::string input = message;
    std::string transformed_message =
            input.substr(0, 2) + "-" + input.substr(2, 3) + "-" + input.substr(5, 2) + "\n\r";

    ssize_t bytes_written = write(port, transformed_message.c_str(), transformed_message.length());
    if (bytes_written == -1) {
        LOG4CPLUS_ERROR(logger, "Error writing to serial port.");
    } else {
        LOG4CPLUS_INFO(logger, "Message sent to serial port.");
    }
}


void processEntry(std::unordered_map<std::string, int> &tableau, const std::string &entree) {
    number_of_plates_total++;

    const std::regex pattern("^[A-Z]{2}[0-9]{3}[A-Z]{2}$");

    if (!std::regex_match(entree, pattern)) {
        LOG4CPLUS_ERROR(logger, "Plate must be in the format XX000XX.");
        return;
    }

    number_of_plates_in_table++;

    if (tableau.find(entree) != tableau.end()) {
        tableau[entree]++;
    } else {
        tableau[entree] = 1;
    }

    const int MAX_TABLEAU_SIZE = 3;

    if (tableau.size() == MAX_TABLEAU_SIZE) {
        std::string valeurMax;
        int indexMax = 0;

        for (const auto &pair : tableau) {
            if (pair.second > indexMax) {
                indexMax = pair.second;
                valeurMax = pair.first;
            }
        }

        LOG4CPLUS_INFO(logger, "Plate is : " << valeurMax << " - Found after : " << number_of_plates_in_table << "/" << number_of_plates_total " plates");

        number_of_plates_in_table = 0;
        tableau.clear();

        // Socket mode
        // send_message_to_socket(message_with_newline.c_str());

        // RS mode
        send_message_to_serial_port(valeurMax.c_str());
    }
}

int main(int argc, const char **argv) {
    signal(SIGSEGV, segfault_handler); // install our segfault handler
    daemon_active = true;

    bool noDaemon = false;
    bool clockOn = false;
    std::string logFile;

    std::string configDir;

    TCLAP::CmdLine cmd("OpenAlpr Daemon", ' ', Alpr::getVersion());

    TCLAP::ValueArg<std::string> configDirArg("", "config",
                                              "Path to the openalpr config directory that contains alprd.conf and openalpr.conf. (Default: /etc/openalpr/)",
                                              false, "/etc/openalpr/", "config_file");
    TCLAP::ValueArg<std::string> logFileArg("l", "log", "Log file to write to.  Default=" + DEFAULT_LOG_FILE_PATH,
                                            false, DEFAULT_LOG_FILE_PATH, "topN");

    TCLAP::SwitchArg daemonOffSwitch("f", "foreground",
                                     "Set this flag for debugging.  Disables forking the process as a daemon and runs in the foreground.  Default=off",
                                     cmd, false);
    TCLAP::SwitchArg clockSwitch("", "clock", "Display timing information to log.  Default=off", cmd, false);

    init_server_socket();
    try {

        cmd.add(configDirArg);
        cmd.add(logFileArg);

        if (!cmd.parse(argc, argv)) {
            // Error occurred while parsing.  Exit now.
            return 1;
        }

        // Make sure configDir ends in a slash
        configDir = configDirArg.getValue();
        if (!hasEnding(configDir, "/"))
            configDir = configDir + "/";

        logFile = logFileArg.getValue();
        noDaemon = daemonOffSwitch.getValue();
        clockOn = clockSwitch.getValue();
    }
    catch (TCLAP::ArgException &e) // catch any exceptions
    {
        std::cerr << "error: " << e.error() << " for arg " << e.argId() << std::endl;
        return 1;
    }

    std::string openAlprConfigFile = configDir + OPENALPR_CONFIG_FILE_NAME;
    std::string daemonConfigFile = configDir + ALPRD_CONFIG_FILE_NAME;

    // Validate that the configuration files exist
    if (!fileExists(openAlprConfigFile.c_str())) {
        std::cerr << "error, openalpr.conf file does not exist at: " << openAlprConfigFile << std::endl;
        return 1;
    }
    if (!fileExists(daemonConfigFile.c_str())) {
        std::cerr << "error, alprd.conf file does not exist at: " << daemonConfigFile << std::endl;
        return 1;
    }

    log4cplus::BasicConfigurator config;
    config.configure();

    if (!noDaemon) {
        // Fork off into a separate daemon
        daemon(0, 0);

        log4cplus::SharedAppenderPtr myAppender(new log4cplus::RollingFileAppender(logFile));
        myAppender->setName("alprd_appender");
        // Redirect std out to log file
        logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("alprd"));
        logger.addAppender(myAppender);

        LOG4CPLUS_INFO(logger, "Running OpenALPR daemon in daemon mode.");
    } else {
        // log4cplus::SharedAppenderPtr myAppender(new log4cplus::ConsoleAppender());
        // myAppender->setName("alprd_appender");
        //  Redirect std out to log file
        logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("alprd"));
        // logger.addAppender(myAppender);

        LOG4CPLUS_INFO(logger, "Running OpenALPR daemon in the foreground.");
    }

    LOG4CPLUS_INFO(logger, "Using: " << daemonConfigFile << " for daemon configuration");

    std::string
            daemon_defaults_file = INSTALL_PREFIX
    "/share/openalpr/config/alprd.defaults.conf";
    DaemonConfig daemon_config(daemonConfigFile, daemon_defaults_file);

    if (daemon_config.stream_urls.size() == 0) {
        LOG4CPLUS_FATAL(logger, "No video streams defined in the configuration.");
        return 1;
    }

    LOG4CPLUS_INFO(logger, "Using: " << daemon_config.imageFolder << " for storing valid plate images");

    pid_t pid;

    std::vector < tthread::thread * > threads;

    for (int i = 0; i < daemon_config.stream_urls.size(); i++) {
        pid = fork();
        if (pid == (pid_t) 0) {
            // This is the child process, kick off the capture data and upload threads
            CaptureThreadData *tdata = new CaptureThreadData();
            tdata->stream_url = daemon_config.stream_urls[i];
            tdata->camera_id = i + 1;
            tdata->config_file = openAlprConfigFile;
            tdata->output_images = daemon_config.storePlates;
            tdata->output_image_folder = daemon_config.imageFolder;
            tdata->country_code = daemon_config.country;
            tdata->company_id = daemon_config.company_id;
            tdata->site_id = daemon_config.site_id;
            tdata->analysis_threads = daemon_config.analysis_threads;
            tdata->top_n = daemon_config.topn;
            tdata->pattern = daemon_config.pattern;
            tdata->clock_on = clockOn;

            tthread::thread *thread_recognize = new tthread::thread(streamRecognitionThread, (void *) tdata);
            threads.push_back(thread_recognize);

            break;
        }
        // Parent process will continue and spawn more children
    }

    while (daemon_active)
        alpr::sleep_ms(30);

    for (uint16_t i = 0; i < threads.size(); i++)
        delete threads[i];

    return 0;
}

void processingThread(void *arg) {
    CaptureThreadData *tdata = (CaptureThreadData *) arg;
    Alpr alpr(tdata->country_code, tdata->config_file);
    alpr.setTopN(tdata->top_n);
    alpr.setDefaultRegion(tdata->pattern);

    while (daemon_active) {

        // Wait for a new frame
        cv::Mat frame = framesQueue.pop();

        // Process new frame
        timespec startTime;
        getTimeMonotonic(&startTime);

        std::vector <AlprRegionOfInterest> regionsOfInterest;
        regionsOfInterest.push_back(AlprRegionOfInterest(0, 0, frame.cols, frame.rows));

        AlprResults results = alpr.recognize(frame.data, frame.elemSize(), frame.cols, frame.rows, regionsOfInterest);

        timespec endTime;
        getTimeMonotonic(&endTime);
        double totalProcessingTime = diffclock(startTime, endTime);

        if (tdata->clock_on) {
            LOG4CPLUS_INFO(logger,
                           "Camera " << tdata->camera_id << " processed frame in: " << totalProcessingTime << " ms.");
        }

        if (results.plates.size() > 0) {

            std::stringstream uuid_ss;
            uuid_ss << tdata->site_id << "-cam" << tdata->camera_id << "-" << getEpochTimeMs();
            std::string uuid = uuid_ss.str();

            // Save the image to disk (using the UUID)
            if (tdata->output_images) {
                std::stringstream ss;
                ss << tdata->output_image_folder << "/" << uuid << ".jpg";
                cv::imwrite(ss.str(), frame);
            }

            // Update the JSON content to include UUID and camera ID
            std::string json = alpr.toJson(results);
            cJSON *root = cJSON_Parse(json.c_str());
            cJSON_AddStringToObject(root, "uuid", uuid.c_str());
            cJSON_AddNumberToObject(root, "camera_id", tdata->camera_id);
            cJSON_AddStringToObject(root, "site_id", tdata->site_id.c_str());
            cJSON_AddNumberToObject(root, "img_width", frame.cols);
            cJSON_AddNumberToObject(root, "img_height", frame.rows);

            // Add the company ID to the output if configured
            if (tdata->company_id.length() > 0)
                cJSON_AddStringToObject(root, "company_id", tdata->company_id.c_str());

            char *out;
            out = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);

            std::string response(out);

            free(out);

            for (int j = 0; j < results.plates.size(); j++) {
                LOG4CPLUS_DEBUG(logger, "Plate detected: " << results.plates[j].bestPlate.characters << " - " << results.plates[j].bestPlate.overall_confidence);
                std::string plate_string(results.plates[j].bestPlate.characters);
                processEntry(tableau, plate_string);
            }
        }
        usleep(10000);
    }
}

void streamRecognitionThread(void *arg) {
    CaptureThreadData *tdata = (CaptureThreadData *) arg;

    LOG4CPLUS_INFO(logger, "country: " << tdata->country_code << " -- config file: " << tdata->config_file);
    LOG4CPLUS_INFO(logger, "pattern: " << tdata->pattern);
    LOG4CPLUS_INFO(logger, "Stream " << tdata->camera_id << ": " << tdata->stream_url);

    /* Create processing threads */
    const int num_threads = tdata->analysis_threads;
    tthread::thread *threads[num_threads];

    for (int i = 0; i < num_threads; i++) {
        LOG4CPLUS_INFO(logger, "Spawning Thread " << i);
        tthread::thread *t = new tthread::thread(processingThread, (void *) tdata);
        threads[i] = t;
    }

    cv::Mat frame;
    LoggingVideoBuffer videoBuffer(logger);
    videoBuffer.connect(tdata->stream_url, 5);
    LOG4CPLUS_INFO(logger, "Starting camera " << tdata->camera_id);

    while (daemon_active) {
        std::vector <cv::Rect> regionsOfInterest;
        int response = videoBuffer.getLatestFrame(&frame, regionsOfInterest);

        if (response != -1) {
            if (framesQueue.empty()) {
                framesQueue.push(frame.clone());
            }
        }

        usleep(10000);
    }

    videoBuffer.disconnect();
    LOG4CPLUS_INFO(logger, "Video processing ended");
    delete tdata;
    for (int i = 0; i < num_threads; i++) {
        delete threads[i];
    }
}
