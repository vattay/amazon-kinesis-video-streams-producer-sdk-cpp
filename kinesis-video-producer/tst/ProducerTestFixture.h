#pragma GCC diagnostic ignored "-Wwrite-strings"

#include "gtest/gtest.h"
#include "KinesisVideoProducer.h"
#include "DefaultDeviceInfoProvider.h"
#include "ClientCallbackProvider.h"
#include "StreamCallbackProvider.h"
#include "Auth.h"
#include "StreamDefinition.h"

using namespace std;
using namespace std::chrono;

namespace com { namespace amazonaws { namespace kinesis { namespace video {

LOGGER_TAG("com.amazonaws.kinesis.video.TEST");

#define ACCESS_KEY_ENV_VAR  "AWS_ACCESS_KEY_ID"
#define SECRET_KEY_ENV_VAR  "AWS_SECRET_ACCESS_KEY"
#define SESSION_TOKEN_ENV_VAR "AWS_SESSION_TOKEN"
#define DEFAULT_REGION_ENV_VAR "AWS_DEFAULT_REGION"

#define FRAME_DURATION_IN_MICROS                            40000
#define TEST_EXECUTION_DURATION_IN_SECONDS                  3 * 60
#define TEST_STREAM_COUNT                                   3
#define TEST_FRAME_SIZE                                     1000
#define TEST_STREAMING_TOKEN_DURATION_IN_SECONDS            (45 * 60)
#define TEST_STORAGE_SIZE_IN_BYTES                          1024 * 1024 * 1024ull

class TestClientCallbackProvider : public ClientCallbackProvider {
public:
    StorageOverflowPressureFunc getStorageOverflowPressureCallback() override {
        return storageOverflowPressure;
    }

    static STATUS storageOverflowPressure(UINT64 custom_handle, UINT64 remaining_bytes);
};

class TestStreamCallbackProvider : public StreamCallbackProvider {
public:

    StreamConnectionStaleFunc getStreamConnectionStaleCallback() override {
        return streamConnectionStaleHandler;
    };

    StreamErrorReportFunc getStreamErrorReportCallback() override {
        return streamErrorReportHandler;
    };

    DroppedFrameReportFunc getDroppedFrameReportCallback() override {
        return droppedFrameReportHandler;
    };

    StreamClosedFunc getStreamClosedCallback() override {
        return streamClosedHandler;
    }

private:
    static STATUS streamConnectionStaleHandler(UINT64 custom_data, STREAM_HANDLE stream_handle, UINT64 last_buffering_ack);
    static STATUS streamErrorReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle, UINT64 errored_timecode, STATUS status);
    static STATUS droppedFrameReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle, UINT64 dropped_frame_timecode);
    static STATUS streamClosedHandler(UINT64 custom_data, STREAM_HANDLE stream_handle);
};

class TestDeviceInfoProvider : public DefaultDeviceInfoProvider {
public:
    device_info_t getDeviceInfo() override {
        auto device_info = DefaultDeviceInfoProvider::getDeviceInfo();

        // Set the storage size to 256mb
        device_info.storageInfo.storageSize = TEST_STORAGE_SIZE_IN_BYTES;

        return device_info;
    }
};

class TestCredentialProvider : public StaticCredentialProvider {
    // Test rotation period is 40 second for the grace period.
    const std::chrono::duration<uint64_t> ROTATION_PERIOD = std::chrono::seconds(TEST_STREAMING_TOKEN_DURATION_IN_SECONDS);
public:
    TestCredentialProvider(const Credentials& credentials) :
            StaticCredentialProvider(credentials) {}

    void updateCredentials(Credentials& credentials) override {
        // Copy the stored creds forward
        credentials = credentials_;

        // Update only the expiration
        auto now_time = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now().time_since_epoch());
        auto expiration_seconds = now_time + ROTATION_PERIOD;
        credentials.setExpiration(std::chrono::seconds(expiration_seconds.count()));
        LOG_INFO("New credentials expiration is " << credentials.getExpiration().count());
    }
};

class ProducerTestBase;
extern ProducerTestBase* gProducerApiTest;
class ProducerTestBase : public ::testing::Test {
public:
    ProducerTestBase() : producer_thread_(0),
                         start_producer_(false),
                         stop_called_(false),
                         stop_producer_(false),
                         defaultRegion_(DEFAULT_AWS_REGION) {

        // Set the global to this object so we won't need to allocate structures in the heap
        gProducerApiTest = this;

        device_provider_ = make_unique<TestDeviceInfoProvider>();
        client_callback_provider_ = make_unique<TestClientCallbackProvider>();
        stream_callback_provider_ = make_unique<TestStreamCallbackProvider>();

        // Read the credentials from the environmental variables if defined. Use defaults if not.
        char const *accessKey;
        char const *secretKey;
        char const *sessionToken;
        char const *defaultRegion;
        string sessionTokenStr;
        if (nullptr == (accessKey = getenv(ACCESS_KEY_ENV_VAR))) {
            accessKey = "AccessKey";
        }

        if (nullptr == (secretKey = getenv(SECRET_KEY_ENV_VAR))) {
            secretKey = "SecretKey";
        }

        if (nullptr == (sessionToken = getenv(SESSION_TOKEN_ENV_VAR))) {
            sessionTokenStr = "";
        } else {
            sessionTokenStr = string(sessionToken);
        }

        if (nullptr != (defaultRegion = getenv(DEFAULT_REGION_ENV_VAR))) {
            defaultRegion_ = string(defaultRegion);
        }

        credentials_ = make_unique<Credentials>(string(accessKey),
                                                string(secretKey),
                                                sessionTokenStr,
                                                std::chrono::seconds(TEST_STREAMING_TOKEN_DURATION_IN_SECONDS));

        credential_provider_ = make_unique<TestCredentialProvider>(*credentials_.get());
    }

    PVOID basicProducerRoutine(KinesisVideoStream*);

    volatile bool stop_called_;

protected:

    void CreateProducer() {
        // Create the producer client
        kinesis_video_producer_ = KinesisVideoProducer::createSync(move(device_provider_),
                                                                   move(client_callback_provider_),
                                                                   move(stream_callback_provider_),
                                                                   move(credential_provider_),
                                                                   defaultRegion_);
    };

    unique_ptr<KinesisVideoStream> CreateTestStream(int index) {
        char stream_name[MAX_STREAM_NAME_LEN];
        sprintf(stream_name, "ScaryTestStream_%d", index);
        map<string, string> tags;
        char tag_name[MAX_TAG_NAME_LEN];
        char tag_val[MAX_TAG_VALUE_LEN];
        for (int i = 0; i < 5; i++) {
            sprintf(tag_name, "testTag_%d_%d", index, i);
            sprintf(tag_val, "testTag_%d_%d_Value", index, i);

            tags.emplace(std::make_pair(tag_name, tag_val));
        }

        auto stream_definition = make_unique<StreamDefinition>(stream_name,
                                                               hours(2),
                                                               &tags,
                                                               "",
                                                               STREAMING_TYPE_REALTIME,
                                                               "video/h264",
                                                               milliseconds::zero(),
                                                               seconds(2),
                                                               milliseconds(1),
                                                               true,
                                                               true,
                                                               false);
        return kinesis_video_producer_->createStreamSync(move(stream_definition));
    };

    virtual void SetUp() {
        LOG_INFO("Setting up test: " << GetTestName());
        CreateProducer();
    };

    virtual void TearDown() {
        LOG_INFO("Tearing down test: " << GetTestName());
    };

    PCHAR GetTestName() {
        return (PCHAR) ::testing::UnitTest::GetInstance()->current_test_info()->test_case_name();
    };

    unique_ptr<KinesisVideoProducer> kinesis_video_producer_;
    unique_ptr<DeviceInfoProvider> device_provider_;
    unique_ptr<ClientCallbackProvider> client_callback_provider_;
    unique_ptr<StreamCallbackProvider> stream_callback_provider_;
    unique_ptr<Credentials> credentials_;
    unique_ptr<CredentialProvider> credential_provider_;

    string defaultRegion_;

    pthread_t producer_thread_;
    volatile bool start_producer_;
    volatile bool stop_producer_;

    BYTE frameBuffer[TEST_FRAME_SIZE];
};

}  // namespace video
}  // namespace kinesis
}  // namespace amazonaws
}  // namespace com;
