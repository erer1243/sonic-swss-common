#include <string>
#include <deque>
#include <limits>
#include <hiredis/hiredis.h>
#include <zmq.h>
#include <pthread.h>
#include "dbconnector.h"
#include "table.h"
#include "selectable.h"
#include "selectableevent.h"
#include "redisselect.h"
#include "redisapi.h"
#include "zmqconsumerstatetable.h"
#include "binaryserializer.h"

using namespace std;

namespace swss {

ZmqConsumerStateTable::ZmqConsumerStateTable(DBConnector *db, const std::string &tableName, ZmqServer &zmqServer, int popBatchSize, int pri, bool dbPersistence)
    : Selectable(pri)
    , TableBase(tableName, TableBase::getTableSeparator(db->getDbId()))
    , m_db(db)
    , m_zmqServer(zmqServer)
{
    if (dbPersistence)
    {
        SWSS_LOG_DEBUG("Database persistence enabled, tableName: %s", tableName.c_str());
        m_asyncDBUpdater = std::make_unique<AsyncDBUpdater>(db, tableName);
    }
    else
    {
        SWSS_LOG_DEBUG("Database persistence disabled, tableName: %s", tableName.c_str());
        m_asyncDBUpdater = nullptr;
    }

    m_zmqServer.registerMessageHandler(m_db->getDbName(), tableName, this);

    SWSS_LOG_DEBUG("ZmqConsumerStateTable ctor tableName: %s", tableName.c_str());
}

void ZmqConsumerStateTable::handleReceivedData(const std::vector<std::shared_ptr<KeyOpFieldsValuesTuple>> &kcos)
{
    for (auto kco : kcos)
    {
        std::shared_ptr<KeyOpFieldsValuesTuple> clone = nullptr;
        if (m_asyncDBUpdater != nullptr)
        {
            // clone before put to received queue, because received data may change by consumer.
            clone = std::make_shared<KeyOpFieldsValuesTuple>(*kco);
        }

        {
            std::lock_guard<std::mutex> lock(m_receivedQueueMutex);
            m_receivedOperationQueue.push(kco);
        }

        if (m_asyncDBUpdater != nullptr)
        {
            m_asyncDBUpdater->update(clone);
        }
    }
    m_selectableEvent.notify(); // will release epoll
}

/* Get multiple pop elements */
void ZmqConsumerStateTable::pops(std::deque<KeyOpFieldsValuesTuple> &vkco, const std::string& /*prefix*/)
{
    queue<shared_ptr<KeyOpFieldsValuesTuple>> q;
    {
        lock_guard<mutex> l(m_receivedQueueMutex);
        swap(m_receivedOperationQueue, q);
    }

    vkco.clear();
    while (!q.empty()) {
        vkco.push_back(std::move(*q.front()));
        q.pop();
    }
}

size_t ZmqConsumerStateTable::dbUpdaterQueueSize()
{
    if (m_asyncDBUpdater == nullptr)
    {
        throw system_error(make_error_code(errc::operation_not_supported),
                           "Database persistence is not enabled");
    }

    return m_asyncDBUpdater->queueSize();
}

}
