/**
 * @file
 * Implementation of class for launching a bundled router
 */

/******************************************************************************
 * Copyright AllSeen Alliance. All rights reserved.
 *
 *    Permission to use, copy, modify, and/or distribute this software for any
 *    purpose with or without fee is hereby granted, provided that the above
 *    copyright notice and this permission notice appear in all copies.
 *
 *    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 ******************************************************************************/

#include <stdio.h>

#include <qcc/platform.h>
#include <qcc/Debug.h>
#include <qcc/Logger.h>
#include <qcc/Log.h>
#include <qcc/String.h>
#include <qcc/StringSource.h>
#include <qcc/StringUtil.h>
#include <qcc/Mutex.h>
#include <qcc/Thread.h>
#include <qcc/FileStream.h>

#include <alljoyn/BusAttachment.h>

#include <alljoyn/Status.h>

#include "BundledRouter.h"
#include "Bus.h"
#include "BusController.h"
#include "ConfigDB.h"
#include "Transport.h"
#include "TCPTransport.h"
#include "UDPTransport.h"
#include "DaemonSLAPTransport.h"
#include "DaemonTransport.h"

#define QCC_MODULE "ALLJOYN_ROUTER"

using namespace qcc;
using namespace std;

namespace ajn {


//    "  <listen>tcp:iface=*,port=0</listen>"
//    "  <listen>udp:iface=*,port=0</listen>"

static const char bundledConfig[] =
    "<busconfig>"
    "  <type>alljoyn_bundled</type>"
    "  <listen>slap:type=uart,dev=/dev/ttyUSB1,baud=115200</listen> "
    "  <listen>unix:abstract=alljoyn</listen>"
    "  <limit name=\"auth_timeout\">20000</limit>"
    "  <limit name=\"max_incomplete_connections\">48</limit>"
    "  <limit name=\"max_completed_connections\">64</limit>"
    "  <limit name=\"max_remote_clients_tcp\">48</limit>"
    "  <limit name=\"max_remote_clients_udp\">48</limit>"
    "  <property name=\"router_power_source\">Battery powered and chargeable</property>"
    "  <property name=\"router_mobility\">Intermediate mobility</property>"
    "  <property name=\"router_availability\">3-6 hr</property>"
    "  <property name=\"router_node_connection\">Wireless</property>"
    "</busconfig>";

/*
 * Router Power Source
 *  Always AC powered
 *  Battery powered and chargeable
 *  Battery powered and not chargeable
 *
 * Router Mobility
 *  Always Stationary
 *  Low mobility
 *  Intermediate mobility
 *  High mobility
 *
 * Router Availability
 *  0-3 hr
 *  3-6 hr
 *  6-9 hr
 *  9-12 hr
 *  12-15 hr
 *  15-18 hr
 *  18-21 hr
 *  21-24 hr
 *
 * Router Node Connection
 *  Access Point
 *  Wired
 *  Wireless
 *
 */

ClientAuthListener::ClientAuthListener()
    : AuthListener(), maxAuth(2)
{
}

bool ClientAuthListener::RequestCredentials(const char* authMechanism, const char* authPeer, uint16_t authCount, const char* userId, uint16_t credMask, Credentials& creds)
{
    QCC_UNUSED(userId);

    if (authCount > maxAuth) {
        return false;
    }

    printf("RequestCredentials for authenticating %s using mechanism %s\n", authPeer, authMechanism);

    if (strcmp(authMechanism, PasswordManager::GetAuthMechanism().c_str()) == 0) {
        if (credMask & AuthListener::CRED_PASSWORD) {
            creds.SetPassword(PasswordManager::GetPassword());
        }
        return true;
    }
    return false;
}

void ClientAuthListener::AuthenticationComplete(const char* authMechanism, const char* authPeer, bool success)
{
    QCC_UNUSED(authMechanism);
    QCC_UNUSED(authPeer);
    QCC_UNUSED(success);

    QCC_DbgPrintf(("Authentication %s %s\n", authMechanism, success ? "succesful" : "failed"));
}

bool ExistFile(const char* fileName) {
    if (fileName) {
        FILE* file = fopen(fileName, "r");
        if (file) {
            fclose(file);
            return true;
        }
    }

    return false;
}

/*
 * Create the singleton bundled router instance.
 *
 * Sidebar on starting a bundled router
 * ====================================
 *
 * How this works is via a fairly non-obvious mechanism, so we describe the
 * process here.  If it is desired to use the bundled router, the user (for
 * example bbclient or bbservice) calls BundledRouterInit().  This will then
 * call the constructor for the BundledRouter object.  The constructor calls
 * into a static method (RegisterRouterLauncher) of the NullTransport to
 * register itself as the router to be launched.  This sets the stage for the
 * use of the bundled router.
 *
 * When the program using the bundled router tries to connect to a bus
 * attachment it calls BusAttachment::Connect().  This tries to connect to an
 * existing router first and if that connect does not succeed, it tries to
 * connect over the NullTransport to the bundled router.
 *
 * The NullTransport::Connect() method looks to see if it (the null transport)
 * is running, and if it is not it looks to see if it has a routerLauncher.
 * Recall that the constructor for the BundledRouter object registered itself as
 * a router launcher, so the null transport will find the launcher since it
 * included the object file corresponding to this source.  The null transport
 * then does a routerLauncher->Start() which calls back into the bundled router
 * object BundledRouter::Start() method below, providing the router with the
 * NullTransport pointer.  The Start() method brings up the bundled router and
 * links the routing node to the bus attachment using the provided null transport.
 *
 * So to summarize, one uses the bundled router simply by calling
 * BundledRouterInit() This creates a bundled router object and registers it with
 * the null transport.  When trying to connect to a router using a bus
 * attachment in the usual way, if there is no currently running native router
 * process, the bus attachment will automagically try to connect to a registered
 * bundled router using the null transport.  This will start the bundled router
 * and then connect to it.
 *
 * Stopping the bundled router happens in the BundledRouterShutdown().
 */

BundledRouter::BundledRouter() : transportsInitialized(false), stopping(false), ajBus(NULL), ajBusController(NULL)
{
    NullTransport::RegisterRouterLauncher(this);
    LoggerSetting::GetLoggerSetting("bundled-router");

    /*
     * Setup the config
     */
#ifdef TEST_CONFIG
    configFile = TEST_CONFIG;
    qcc::String configStr = bundledConfig;
    if (ExistFile(configFile.c_str())) {
        configStr = "";
    } else {
        configFile = "";
    }
    config = new ConfigDB(configStr, configFile);
#else
    config = new ConfigDB(bundledConfig);
#endif
}

BundledRouter::~BundledRouter()
{
    QCC_DbgPrintf(("BundledRouter::~BundledRouter"));
    lock.Lock(MUTEX_CONTEXT);
    while (!transports.empty()) {
        set<NullTransport*>::iterator iter = transports.begin();
        NullTransport* trans = *iter;
        transports.erase(iter);
        lock.Unlock(MUTEX_CONTEXT);
        trans->Disconnect("null:");
        lock.Lock(MUTEX_CONTEXT);
    }
    lock.Unlock(MUTEX_CONTEXT);
    Join();
    delete config;
}

QStatus BundledRouter::Start(NullTransport* nullTransport)
{
    QStatus status = ER_OK;

    QCC_DbgHLPrintf(("Using BundledRouter"));

#ifdef TEST_CONFIG
    if (configFile.size() > 0) {
        QCC_DbgHLPrintf(("Using external config file: %s", configFile.c_str()));
    }
#endif

    /*
     * If the bundled router is in the process of stopping we need to wait until the operation is
     * complete (BundledRouter::Join has exited) before we attempt to start up again.
     */
    lock.Lock(MUTEX_CONTEXT);
    while (stopping) {
        if (!transports.empty()) {
            QCC_ASSERT(transports.empty());
        }
        lock.Unlock(MUTEX_CONTEXT);
        qcc::Sleep(5);
        lock.Lock(MUTEX_CONTEXT);
    }
    if (transports.empty()) {
        if (!config->LoadConfig()) {
            status = ER_BUS_BAD_XML;
            QCC_LogError(status, ("Error parsing configuration"));
            goto ErrorExit;
        }
        /*
         * Extract the listen specs
         */
        const ConfigDB::ListenList listenList = config->GetListen();
        String listenSpecs;
        for (ConfigDB::_ListenList::const_iterator it = listenList->begin(); it != listenList->end(); ++it) {
            if (!listenSpecs.empty()) {
                listenSpecs.append(";");
            }
            listenSpecs.append(*it);
        }
        /*
         * Register the transport factories - this is a one time operation
         */
        if (!transportsInitialized) {
//            Add(new TransportFactory<TCPTransport>(TCPTransport::TransportName, false));
//            Add(new TransportFactory<UDPTransport>(UDPTransport::TransportName, false));
            Add(new TransportFactory<DaemonTransport>(DaemonTransport::TransportName, true)); 
            Add(new TransportFactory<DaemonSLAPTransport>(DaemonSLAPTransport::TransportName, false));
            transportsInitialized = true;
        }
        QCC_DbgPrintf(("Starting bundled router bus attachment"));
        /*
         * Create and start the routing node
         */
        ajBus = new Bus("bundled-router", *this, listenSpecs.c_str());
        if (PasswordManager::GetAuthMechanism() != "ANONYMOUS" && PasswordManager::GetPassword() != "") {
            ajBusController = new BusController(*ajBus, &authListener);
        } else {
            ajBusController = new BusController(*ajBus, NULL);
        }

        status = ajBusController->Init(listenSpecs);
        if (ER_OK != status) {
            goto ErrorExit;
        }
    }
    /*
     * Use the null transport to link the routing node and client bus together
     */
    status = nullTransport->LinkBus(ajBus);
    if (status != ER_OK) {
        goto ErrorExit;
    }

    transports.insert(nullTransport);

    lock.Unlock(MUTEX_CONTEXT);

    return ER_OK;

ErrorExit:

    if (transports.empty()) {
        delete ajBusController;
        ajBusController = NULL;
        delete ajBus;
        ajBus = NULL;
    }
    lock.Unlock(MUTEX_CONTEXT);
    return status;
}

void BundledRouter::Join()
{
    QCC_DbgPrintf(("BundledRouter::Join"));
    lock.Lock(MUTEX_CONTEXT);
    if (transports.empty() && ajBus && ajBusController) {
        QCC_DbgPrintf(("Joining bundled router bus attachment"));
        ajBusController->Join();
        delete ajBusController;
        ajBusController = NULL;
        delete ajBus;
        ajBus = NULL;
        /*
         * Clear the stopping state
         */
        stopping = false;
    }
    lock.Unlock(MUTEX_CONTEXT);
}

QStatus BundledRouter::Stop(NullTransport* nullTransport)
{
    QCC_DbgPrintf(("BundledRouter::Stop"));
    lock.Lock(MUTEX_CONTEXT);
    transports.erase(nullTransport);
    QStatus status = ER_OK;
    if (transports.empty()) {
        /*
         * Set the stopping state to block any calls to Start until
         * after Join() has been called.
         */
        stopping = true;
        if (ajBusController) {
            status = ajBusController->Stop();
        }
    }
    lock.Unlock(MUTEX_CONTEXT);
    return status;
}

} // namespace ajn
