// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "ExternalCANDataSource.h"
#include "CANDataConsumer.h"
#include "CollectionInspectionAPITypes.h"
#include "IDecoderDictionary.h"
#include "MessageTypes.h"
#include "QueueTypes.h"
#include "SignalTypes.h"
#include "TimeTypes.h"
#include "VehicleDataSourceTypes.h"
#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <linux/can.h>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Aws
{
namespace IoTFleetWise
{

static void
sendTestMessage( ExternalCANDataSource &dataSource,
                 CANChannelNumericID channelId,
                 uint32_t messageId = 0x123,
                 Timestamp timestamp = 0 )
{
    std::vector<uint8_t> data( 8 );
    for ( uint8_t i = 0; i < 8; ++i )
    {
        data[i] = i;
    }
    dataSource.ingestMessage( channelId, timestamp, messageId, data );
}

static void
sendTestFDMessage( ExternalCANDataSource &dataSource,
                   CANChannelNumericID channelId,
                   uint32_t messageId = 0x123,
                   Timestamp timestamp = 0 )
{
    std::vector<uint8_t> data( 64 );
    for ( uint8_t i = 0; i < 64; ++i )
    {
        data[i] = i;
    }
    dataSource.ingestMessage( channelId, timestamp, messageId, data );
}

static void
sendTestMessageExtendedID( ExternalCANDataSource &dataSource,
                           CANChannelNumericID channelId,
                           uint32_t messageId = 0x123,
                           Timestamp timestamp = 0 )
{
    std::vector<uint8_t> data( 8 );
    for ( uint8_t i = 0; i < 8; ++i )
    {
        data[i] = i;
    }
    dataSource.ingestMessage( channelId, timestamp, messageId | CAN_EFF_FLAG, data );
}

class ExternalCANDataSourceTest : public ::testing::Test
{
protected:
    void
    SetUp() override
    {
        std::unordered_map<CANRawFrameID, CANMessageDecoderMethod> frameMap;
        CANMessageDecoderMethod decoderMethod;
        decoderMethod.collectType = CANMessageCollectType::RAW_AND_DECODE;

        decoderMethod.format.mMessageID = 0x123;
        decoderMethod.format.mSizeInBytes = 8;

        CANSignalFormat sigFormat1;
        sigFormat1.mSignalID = 1;
        sigFormat1.mIsBigEndian = true;
        sigFormat1.mIsSigned = true;
        sigFormat1.mFirstBitPosition = 24;
        sigFormat1.mSizeInBits = 30;
        sigFormat1.mOffset = 0.0;
        sigFormat1.mFactor = 1.0;
        sigFormat1.mSignalType = SignalType::DOUBLE;

        CANSignalFormat sigFormat2;
        sigFormat2.mSignalID = 7;
        sigFormat2.mIsBigEndian = true;
        sigFormat2.mIsSigned = true;
        sigFormat2.mFirstBitPosition = 56;
        sigFormat2.mSizeInBits = 31;
        sigFormat2.mOffset = 0.0;
        sigFormat2.mFactor = 1.0;
        sigFormat2.mSignalType = SignalType::DOUBLE;

        decoderMethod.format.mSignals.push_back( sigFormat1 );
        decoderMethod.format.mSignals.push_back( sigFormat2 );
        frameMap[0x123] = decoderMethod;
        mDictionary = std::make_shared<CANDecoderDictionary>();
        mDictionary->canMessageDecoderMethod[0] = frameMap;
        mDictionary->signalIDsToCollect.emplace( 1 );
        mDictionary->signalIDsToCollect.emplace( 7 );
    }

    void
    TearDown() override
    {
    }

    std::shared_ptr<CANDecoderDictionary> mDictionary;
};

TEST_F( ExternalCANDataSourceTest, testNoDecoderDictionary )
{
    auto signalBuffer = std::make_shared<SignalBuffer>( 10, "Signal Buffer" );
    auto signalBufferDistributor = std::make_shared<SignalBufferDistributor>();
    signalBufferDistributor->registerQueue( signalBuffer );

    CANDataConsumer consumer{ signalBufferDistributor };
    ExternalCANDataSource dataSource{ consumer };
    CollectedDataFrame collectedDataFrame;
    sendTestMessage( dataSource, 0 );
    ASSERT_FALSE( signalBuffer->pop( collectedDataFrame ) );
}

TEST_F( ExternalCANDataSourceTest, testValidDecoderDictionary )
{
    auto signalBuffer = std::make_shared<SignalBuffer>( 10, "Signal Buffer" );

    auto signalBufferDistributor = std::make_shared<SignalBufferDistributor>();
    signalBufferDistributor->registerQueue( signalBuffer );

    CANDataConsumer consumer{ signalBufferDistributor };
    ExternalCANDataSource dataSource{ consumer };
    dataSource.onChangeOfActiveDictionary( mDictionary, VehicleDataSourceProtocol::RAW_SOCKET );
    CollectedDataFrame collectedDataFrame;
    sendTestMessage( dataSource, 0 );
    ASSERT_TRUE( signalBuffer->pop( collectedDataFrame ) );
    auto signal = collectedDataFrame.mCollectedSignals[0];
    ASSERT_EQ( signal.value.type, SignalType::DOUBLE );
    ASSERT_EQ( signal.signalID, 1 );
    ASSERT_DOUBLE_EQ( signal.value.value.doubleVal, 0x10203 );
    signal = collectedDataFrame.mCollectedSignals[1];
    ASSERT_EQ( signal.signalID, 7 );
    ASSERT_EQ( signal.value.type, SignalType::DOUBLE );
    ASSERT_DOUBLE_EQ( signal.value.value.doubleVal, 0x4050607 );
    auto frame = collectedDataFrame.mCollectedCanRawFrame;
    ASSERT_EQ( frame->channelId, 0 );
    ASSERT_EQ( frame->frameID, 0x123 );
    ASSERT_EQ( frame->size, 8 );
    for ( auto i = 0; i < 8; i++ )
    {
        ASSERT_EQ( frame->data[i], i );
    }

    // Test message a different message ID and non-monotonic time is not received
    sendTestMessage( dataSource, 0, 0x456, 1 );
    ASSERT_FALSE( signalBuffer->pop( collectedDataFrame ) );

    // Test invalidation of decoder dictionary
    dataSource.onChangeOfActiveDictionary( nullptr, VehicleDataSourceProtocol::RAW_SOCKET );
    sendTestMessage( dataSource, 0 );
    ASSERT_FALSE( signalBuffer->pop( collectedDataFrame ) );
    // Check it ignores dictionaries for other protocols
    dataSource.onChangeOfActiveDictionary( mDictionary, VehicleDataSourceProtocol::OBD );
    sendTestMessage( dataSource, 0 );
    ASSERT_FALSE( signalBuffer->pop( collectedDataFrame ) );
}

TEST_F( ExternalCANDataSourceTest, testCanFDSocketMode )
{
    auto signalBuffer = std::make_shared<SignalBuffer>( 10, "Signal Buffer" );

    auto signalBufferDistributor = std::make_shared<SignalBufferDistributor>();
    signalBufferDistributor->registerQueue( signalBuffer );

    CANDataConsumer consumer{ signalBufferDistributor };
    ExternalCANDataSource dataSource{ consumer };
    dataSource.onChangeOfActiveDictionary( mDictionary, VehicleDataSourceProtocol::RAW_SOCKET );
    CollectedDataFrame collectedDataFrame;
    sendTestFDMessage( dataSource, 0 );
    ASSERT_TRUE( signalBuffer->pop( collectedDataFrame ) );
    auto signal = collectedDataFrame.mCollectedSignals[0];
    ASSERT_EQ( signal.value.type, SignalType::DOUBLE );
    ASSERT_EQ( signal.signalID, 1 );
    ASSERT_DOUBLE_EQ( signal.value.value.doubleVal, 0x10203 );

    signal = collectedDataFrame.mCollectedSignals[1];
    ASSERT_EQ( signal.signalID, 7 );
    ASSERT_EQ( signal.value.type, SignalType::DOUBLE );
    ASSERT_DOUBLE_EQ( signal.value.value.doubleVal, 0x4050607 );

    auto frame = collectedDataFrame.mCollectedCanRawFrame;
    ASSERT_EQ( frame->channelId, 0 );
    ASSERT_EQ( frame->frameID, 0x123 );
    ASSERT_EQ( frame->size, 64 );
    for ( auto i = 0; i < 64; i++ )
    {
        ASSERT_EQ( frame->data[i], i );
    }
    ASSERT_FALSE( signalBuffer->pop( collectedDataFrame ) );
}

TEST_F( ExternalCANDataSourceTest, testExtractExtendedID )
{
    auto signalBuffer = std::make_shared<SignalBuffer>( 10, "Signal Buffer" );

    auto signalBufferDistributor = std::make_shared<SignalBufferDistributor>();
    signalBufferDistributor->registerQueue( signalBuffer );

    CANDataConsumer consumer{ signalBufferDistributor };
    ExternalCANDataSource dataSource{ consumer };
    dataSource.onChangeOfActiveDictionary( mDictionary, VehicleDataSourceProtocol::RAW_SOCKET );
    CollectedDataFrame collectedDataFrame;
    sendTestMessageExtendedID( dataSource, 0 );
    ASSERT_TRUE( signalBuffer->pop( collectedDataFrame ) );
    auto signal = collectedDataFrame.mCollectedSignals[0];
    ASSERT_EQ( signal.value.type, SignalType::DOUBLE );
    ASSERT_EQ( signal.signalID, 1 );
    ASSERT_DOUBLE_EQ( signal.value.value.doubleVal, 0x10203 );

    signal = collectedDataFrame.mCollectedSignals[1];
    ASSERT_EQ( signal.signalID, 7 );
    ASSERT_EQ( signal.value.type, SignalType::DOUBLE );
    ASSERT_DOUBLE_EQ( signal.value.value.doubleVal, 0x4050607 );

    auto frame = collectedDataFrame.mCollectedCanRawFrame;
    ASSERT_EQ( frame->channelId, 0 );
    ASSERT_EQ( frame->frameID, 0x123 );
    ASSERT_EQ( frame->size, 8 );
    for ( auto i = 0; i < 8; i++ )
    {
        ASSERT_EQ( frame->data[i], i );
    }
    ASSERT_FALSE( signalBuffer->pop( collectedDataFrame ) );
}

} // namespace IoTFleetWise
} // namespace Aws
