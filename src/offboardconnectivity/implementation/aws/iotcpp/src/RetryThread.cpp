// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "RetryThread.h"

using namespace Aws::IoTFleetWise::OffboardConnectivityAwsIot;

std::atomic<int> RetryThread::fInstanceCounter( 0 );

RetryThread::RetryThread( IRetryable &retryable, uint32_t startBackoffMs, uint32_t maxBackoffMs )
    : fRetryable( retryable )
    , fStartBackoffMs( startBackoffMs )
    , fMaxBackoffMs( maxBackoffMs )
    , fCurrentWaitTime( 0 )
    , fShouldStop( false )
{
    fInstance = fInstanceCounter++;
}

bool
RetryThread::start()
{
    // Prevent concurrent stop/init
    std::lock_guard<std::mutex> lock( fThreadMutex );
    // On multi core systems the shared variable fShouldStop must be updated for
    // all cores before starting the thread otherwise thread will directly end
    fShouldStop.store( false );
    if ( !fThread.create( doWork, this ) )
    {
        fLogger.trace( "RetryThread::start", " Retry Thread failed to start " );
    }
    else
    {
        fLogger.trace( "RetryThread::start", " Retry Thread started " );
        fThread.setThreadName( "fwCNRetry" + std::to_string( fInstance ) );
    }

    return fThread.isValid();
}

bool
RetryThread::stop()
{
    if ( !fThread.isValid() )
    {
        return true;
    }

    std::lock_guard<std::mutex> lock( fThreadMutex );
    fShouldStop.store( true );
    fLogger.trace( "RetryThread::stop", " Request stop " );
    fWait.notify();
    fThread.release();
    fShouldStop.store( false, std::memory_order_relaxed );
    return !fThread.isActive();
}

void
RetryThread::doWork( void *data )
{
    RetryThread *retryThread = static_cast<RetryThread *>( data );
    retryThread->fCurrentWaitTime = retryThread->fStartBackoffMs;
    while ( !retryThread->fShouldStop )
    {
        RetryStatus result = retryThread->fRetryable.attempt();
        if ( result != RetryStatus::RETRY )
        {
            retryThread->fLogger.trace( "RetryThread::doWork",
                                        " Finished with code " + std::to_string( static_cast<int>( result ) ) );
            retryThread->fRetryable.onFinished( result );
            return;
        }
        retryThread->fLogger.trace( "RetryThread::doWork",
                                    " Current retry time is: " + std::to_string( retryThread->fCurrentWaitTime ) );
        retryThread->fWait.wait( retryThread->fCurrentWaitTime );
        // exponential backoff
        retryThread->fCurrentWaitTime = std::min( retryThread->fCurrentWaitTime * 2, retryThread->fMaxBackoffMs );
    }
    // If thread is shutdown without succeeding signal abort
    retryThread->fLogger.trace( "RetryThread::doWork", " Stop thread with ABORT" );
    retryThread->fRetryable.onFinished( RetryStatus::ABORT );
}