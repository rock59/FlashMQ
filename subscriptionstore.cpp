#include "subscriptionstore.h"

#include "cassert"

#include "rwlockguard.h"


SubscriptionNode::SubscriptionNode(const std::string &subtopic) :
    subtopic(subtopic)
{

}

SubscriptionStore::SubscriptionStore() :
    root(new SubscriptionNode("root")),
    clients_by_id_const(clients_by_id)
{

}

void SubscriptionStore::addSubscription(Client_p &client, const std::string &topic)
{
    const std::list<std::string> subtopics = split(topic, '/');

    RWLockGuard lock_guard(&subscriptionsRwlock);
    lock_guard.wrlock();

    SubscriptionNode *deepestNode = root.get();
    for(const std::string &subtopic : subtopics)
    {
        SubscriptionNode &nodeRef = *deepestNode;
        std::unique_ptr<SubscriptionNode> &node = nodeRef.children[subtopic];

        if (!node)
        {
            node.reset(new SubscriptionNode(subtopic));
        }
        deepestNode = node.get();
    }

    if (deepestNode)
    {
        deepestNode->subscribers.push_front(client->getClientId());
    }

    clients_by_id[client->getClientId()] = client;
    lock_guard.unlock();

    giveClientRetainedMessage(client, topic); // TODO: wildcards
}

void SubscriptionStore::removeClient(const Client_p &client)
{
    RWLockGuard lock_guard(&subscriptionsRwlock);
    lock_guard.wrlock();
    clients_by_id.erase(client->getClientId());
}

// TODO: should I implement cache, this needs to be changed to returning a list of clients.
bool SubscriptionStore::publishNonRecursively(const MqttPacket &packet, const std::forward_list<std::string> &subscribers) const
{
    bool result = false;

    for (const std::string &client_id : subscribers)
    {
        auto client_it = clients_by_id_const.find(client_id);
        if (client_it != clients_by_id_const.end())
        {
            client_it->second->writeMqttPacket(packet);
            result = true;
        }
    }

    return result;
}

bool SubscriptionStore::publishRecursively(std::list<std::string>::const_iterator cur_subtopic_it, std::list<std::string>::const_iterator end,
                                           std::unique_ptr<SubscriptionNode> &this_node, const MqttPacket &packet) const
{
    if (cur_subtopic_it == end) // This is the end of the topic path, so look for subscribers here.
    {
        publishNonRecursively(packet, this_node->subscribers);
        return true;
    }

    std::string cur_subtop = *cur_subtopic_it;
    auto sub_node = this_node->children.find(cur_subtop);

    const auto next_subtopic = ++cur_subtopic_it;

    if (sub_node != this_node->children.end())
    {
        publishRecursively(next_subtopic, end, sub_node->second, packet);
    }

    const auto plus_sign_node = this_node->children.find("+");

    if (plus_sign_node != this_node->children.end())
    {
        publishRecursively(next_subtopic, end, plus_sign_node->second, packet);
    }

    const auto pound_sign_node = this_node->children.find("#");

    if (pound_sign_node != this_node->children.end())
    {
        return publishNonRecursively(packet, pound_sign_node->second->subscribers);
    }

    return false;
}

void SubscriptionStore::queuePacketAtSubscribers(const std::string &topic, const MqttPacket &packet, const Client_p &sender)
{
    // TODO: keep a cache of topics vs clients

    const std::list<std::string> subtopics = split(topic, '/');

    RWLockGuard lock_guard(&subscriptionsRwlock);
    lock_guard.rdlock();

    publishRecursively(subtopics.begin(), subtopics.end(), root, packet);
}

void SubscriptionStore::giveClientRetainedMessage(Client_p &client, const std::string &topic)
{
    RWLockGuard locker(&retainedMessagesRwlock);
    locker.rdlock();

    auto retained_ptr = retainedMessages.find(topic);

    if (retained_ptr == retainedMessages.end())
        return;

    const RetainedPayload &m = retained_ptr->second;

    Publish publish(topic, m.payload, m.qos);
    publish.retain = true;
    const MqttPacket packet(publish);
    client->writeMqttPacket(packet);
}

void SubscriptionStore::setRetainedMessage(const std::string &topic, const std::string &payload, char qos)
{
    RWLockGuard locker(&retainedMessagesRwlock);
    locker.wrlock();

    auto retained_ptr = retainedMessages.find(topic);
    bool retained_found = retained_ptr != retainedMessages.end();

    if (!retained_found && payload.empty())
        return;

    if (retained_found && payload.empty())
    {
        retainedMessages.erase(topic);
        return;
    }

    RetainedPayload &m = retainedMessages[topic];
    m.payload = payload;
    m.qos = qos;
}


