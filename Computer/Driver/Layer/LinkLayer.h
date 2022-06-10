#ifndef _COMPUTER_DRIVER_LAYER_LINK_LAYER_H_
#define _COMPUTER_DRIVER_LAYER_LINK_LAYER_H_

#include "DataType.h"
#include "../../../DataStructures/CircularQueue.h"
#include "../../../DataStructures/DataBuffer.h"
#include "../../../DataStructures/MACAddress.h"
#include "../../../General/Timer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <queue>
#include <mutex>
#include <thread>
#include <list>
#include <map>

class Configuration;
class NetworkDriver;

class LinkLayer
{
private:
    enum class EventType
    {
        INVALID, // Un evenement invalide
        ACK_TIMEOUT, // On doit envoyer un ack, parce qu'on n'a pas fait de piggybacking
        SEND_TIMEOUT, // On n'a pas recu de reponse du receveur, on doit reenvoyer la trame
        ACK_RECEIVED, // On a recu un ACK
        NAK_RECEIVED, // On a recu un NAK
        SEND_ACK_REQUEST, // On doit envoyer ce ACK
        SEND_NAK_REQUEST, // On doit envoyer ce NAK
        STOP_ACK_TIMER_REQUEST, // On veut arreter les timers de ACK pour une adresse particuliere
    };

    struct Event
    {
        EventType Type = EventType::INVALID;
        size_t Number = 0;
        size_t TimerID = 0;
        MACAddress Address;
        NumberSequence Next = 0;

        static Event Invalid()
        {
            return Event();
        }
    };

    // Custom implementation of SlidingWindow DataType
    struct SlidingWindow
    {
        size_t SIZE = 0;    // Total capacity of the buffer
        size_t WindowLength = 0;    // Size of the SlidingWindow
        size_t WindowCapacity = 0;
        NumberSequence NextID = 0;  // ID of the next frame
        NumberSequence TrameAttendue = 0;
        NumberSequence LowExpected = 0; // Bottom of the window
        NumberSequence HighExpected = 0; // Top of the window
        std::pair<bool, std::pair<size_t, Frame>> Data[100];    // Bool represent if the entry is valid or not

        // Placeholder entry in the window
        std::pair<bool, std::pair<size_t, Frame>> InvalidEntry(){
            return std::make_pair(false, std::make_pair(-1, Frame()));
        }

        void DeleteEntry(NumberSequence pos){
            if (WindowLength == 0) return;

            Data[pos] = InvalidEntry();
            WindowLength--;
        }

        void AddEntry(NumberSequence pos, size_t timerID, const Frame& frame){
            if (WindowLength == WindowCapacity) return;
            
            Data[pos] = std::make_pair(true, std::make_pair(timerID, frame));
            WindowLength++;
        }

        void SwitchTimer(NumberSequence pos, size_t timerID){
            auto entry = Data[pos];
            if (entry.first == false) return;

            Frame frame = entry.second.second;
            Data[pos] = std::make_pair(true, std::make_pair(timerID, frame));
        }

        void UpdateNextID(){
            NextID = (NextID + 1) % SIZE;
        }

        bool isFull(){
            return WindowLength == WindowCapacity;
        }

        void inc(){
            TrameAttendue = (TrameAttendue + 1) % SIZE;
            LowExpected = (LowExpected + 1) % SIZE;
            HighExpected = (HighExpected + 1) % SIZE;
        }
    };

    NetworkDriver* m_driver;
    std::unique_ptr<Timer> m_timers;

    MACAddress m_address;

    // NumberSequence m_maximumSequence;
    NumberSequence m_maximumBufferedFrameCount;
    
    std::chrono::milliseconds m_transmissionTimeout;
    std::chrono::milliseconds m_ackTimeout;
    std::queue<Event> m_receivingEventQueue;
    std::queue<Event> m_sendingEventQueue;

    CircularQueue m_sendingQueue;
    CircularQueue m_receivingQueue;

    std::atomic<bool> m_executeReceiving;
    std::atomic<bool> m_executeSending;

    std::mutex m_mutex;
    std::mutex m_receiveEventMutex;
    std::mutex m_sendEventMutex;

    std::thread m_senderThread;
    std::thread m_receiverThread;

    SlidingWindow m_slidingWindow;

    void receiverCallback();
    void senderCallback();

    bool canSendData(const Frame& data) const;
    bool between(NumberSequence value, NumberSequence first, NumberSequence last) const;

    void sendAck(const MACAddress& to, NumberSequence ackNumber);
    void sendNak(const MACAddress& to, NumberSequence nakNumber);
    bool sendFrame(const Frame& frame);

    void notifyNAK(const Frame& frame);
    void notifyACK(const Frame& frame, NumberSequence piggybackAck);

    void transmissionTimeout(size_t timerID, NumberSequence numberData);
    void ackTimeout(size_t timerID, NumberSequence numberData);

    size_t startAckTimer(size_t existingTimerID, NumberSequence ackNumber);
    void stopAckTimer(size_t timerID);
    void notifyStopAckTimers(const MACAddress& to);

    size_t startTimeoutTimer(NumberSequence number);

    Event getNextSendingEvent();
    Event getNextReceivingEvent();

    MACAddress arp(const Packet& p) const; // Retourne la MACAddress de destination du packet
    bool canReceiveDataFromPhysicalLayer(const Frame& data) const;

public: 
    LinkLayer(NetworkDriver* driver, const Configuration& config);
    ~LinkLayer();

    const MACAddress& getMACAddress() const;

    void start();
    void stop();

    bool dataReady() const;
    Frame getNextData();

    bool dataReceived() const;
    void receiveData(Frame data);

};

#endif //_COMPUTER_DRIVER_LAYER_LINK_LAYER_H_
