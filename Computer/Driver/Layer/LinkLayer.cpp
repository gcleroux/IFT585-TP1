#include "LinkLayer.h"
#include "../NetworkDriver.h"

#include "../../../General/Configuration.h"
#include "../../../General/Logger.h"

#include <iostream>
#include <functional>
#include <map>

bool no_nak = true;

LinkLayer::LinkLayer(NetworkDriver* driver, const Configuration& config)
    : m_driver(driver)
    , m_address(config)
    , m_receivingQueue(config.get(Configuration::LINK_LAYER_RECEIVING_BUFFER_SIZE))
    , m_sendingQueue(config.get(Configuration::LINK_LAYER_SENDING_BUFFER_SIZE))
    , m_maximumBufferedFrameCount(config.get(Configuration::LINK_LAYER_MAXIMUM_BUFFERED_FRAME))
    , m_transmissionTimeout(config.get(Configuration::LINK_LAYER_TIMEOUT))
    , m_executeReceiving(false)
    , m_executeSending(false)
{
    m_maximumSequence = 2 * m_maximumBufferedFrameCount - 1;
    m_ackTimeout = m_transmissionTimeout / 4;
    m_timers = std::make_unique<Timer>();

    NB_BUFS = (m_maximumSequence + 1) / 2;
    m_ackAttendu = 0;
    m_prochaineTrameAEnvoyer = 0;
    m_trameAttendue = 0;
    m_tropLoin = NB_BUFS;
    m_bufferSize = 0;

}

LinkLayer::~LinkLayer()
{
    stop();
    m_driver = nullptr;
}

const MACAddress& LinkLayer::getMACAddress() const
{
    return m_address;
}

// Demarre les fils d'execution pour l'envoi et la reception des trames
void LinkLayer::start()
{
    stop();

    m_timers->start();

    m_executeReceiving = true;
    m_receiverThread = std::thread(&LinkLayer::receiverCallback, this);

    m_executeSending = true;
    m_senderThread = std::thread(&LinkLayer::senderCallback, this);
}

// Arrete les fils d'execution pour l'envoi et la reception des trames
void LinkLayer::stop()
{
    m_timers->stop();

    m_executeReceiving = false;
    if (m_receiverThread.joinable())
    {
        m_receiverThread.join();
    }

    m_executeSending = false;
    if (m_senderThread.joinable())
    {
        m_senderThread.join();
    }
}

// Indique vrai si on peut envoyer des donnees dans le buffer de sortie, faux si le buffer est plein
bool LinkLayer::canSendData(const Frame& data) const
{
    return m_sendingQueue.canWrite<Frame>(data);
}

// Indique vrai si des donnees sont disponibles dans le buffer d'entree, faux s'il n'y a rien
bool LinkLayer::dataReceived() const
{
    return m_receivingQueue.canRead<Frame>();
}

// Indique vrai s'il y a des donnees dans le buffer de sortie
bool LinkLayer::dataReady() const
{
    return m_sendingQueue.canRead<Frame>();
}

// Recupere la prochaine donnee du buffer de sortie
Frame LinkLayer::getNextData()
{
    return m_sendingQueue.pop<Frame>();
}

// Envoit une trame dans le buffer de sortie
// Cette fonction retourne faux si la trame n'a pas ete envoyee. Ce cas arrive seulement si le programme veut se terminer.
// Fait de l'attente active jusqu'a ce qu'il puisse envoyer la trame sinon.
bool LinkLayer::sendFrame(const Frame& frame)
{
    while (m_executeSending)
    {
        if (canSendData(frame))
        {
            // Vous pouvez d�commenter ce code pour avoir plus de d�tails dans la console lors de l'ex�cution
            //Logger log(std::cout);
            //if (frame.Size == FrameType::NAK)
            //{
            //    log << frame.Source << " : Sending NAK  to " << frame.Destination << " : " << frame.Ack << std::endl;
            //}
            //else if (frame.Size == FrameType::ACK)
            //{
            //    log << frame.Source << " : Sending ACK  to " << frame.Destination << " : " << frame.Ack << std::endl;
            //}
            //else
            //{
            //    log << frame.Source << " : Sending DATA to " << frame.Destination << " : " << frame.NumberSeq << std::endl;
            //}
            m_sendingQueue.push(frame);
            return true;
        }
    }
    return false;
}

// Recupere le prochain evenement de communication a gerer pour l'envoi de donnees
LinkLayer::Event LinkLayer::getNextSendingEvent()
{
    std::lock_guard<std::mutex> lock(m_sendEventMutex);
    if (m_sendingEventQueue.size() > 0)
    {
        Event ev = m_sendingEventQueue.front();
        m_sendingEventQueue.pop();
        return ev;
    }
    return Event::Invalid();
}

// Recupere le prochain evenement de communication a gerer pour la reception de donnees
LinkLayer::Event LinkLayer::getNextReceivingEvent()
{
    std::lock_guard<std::mutex> lock(m_receiveEventMutex);
    if (m_receivingEventQueue.size() > 0)
    {
        Event ev = m_receivingEventQueue.front();
        m_receivingEventQueue.pop();
        return ev;
    }
    return Event::Invalid();
}

// Indique si la valeur est comprise entre first et last de facon circulaire
bool LinkLayer::between(NumberSequence value, NumberSequence first, NumberSequence last) const
{
    // Value is between first and last, circular style
    return ((first <= value) && (value < last)) || ((last < first) && (first <= value)) || ((value < last) && (last < first));
}

// Envoit un evenement de communication pour indiquer a l'envoi d'envoyer un ACK
// L'evenement contiendra l'adresse a qui il faut envoyer un ACK et le numero du ACK
void LinkLayer::sendAck(const MACAddress& to, NumberSequence ackNumber)
{
    Event ev = Event::Invalid();
    ev.Type = EventType::SEND_ACK_REQUEST;
    ev.Number = ackNumber;
    ev.Address = to;
    std::lock_guard<std::mutex> lock(m_sendEventMutex);
    m_sendingEventQueue.push(ev);
}

// Envoit un evenement de communication pour indiquer a l'envoi d'envoyer un NAK
// L'evenement contiendra l'adresse a qui il faut envoyer un ACK et le numero du NAK
void LinkLayer::sendNak(const MACAddress& to, NumberSequence nakNumber)
{
    Event ev = Event::Invalid();
    ev.Type = EventType::SEND_NAK_REQUEST;
    ev.Number = nakNumber;
    ev.Address = to;
    std::lock_guard<std::mutex> lock(m_sendEventMutex);
    m_sendingEventQueue.push(ev);
}

// Envoit un evenement de communication pour indiquer a l'envoi qu'on a recu une trame avec potentiellement un ACK (piggybacking)
// L'evenement contiendra l'adresse d'ou provient l'information, le numero du ACK et le prochain ACK qu'on devrait nous-meme envoyer (pour le piggybacking)
void LinkLayer::notifyACK(const Frame& frame, NumberSequence piggybackAck)
{
    Event ev = Event::Invalid();
    ev.Type = EventType::ACK_RECEIVED;
    ev.Number = frame.Ack;
    ev.Address = frame.Source;
    ev.Next = piggybackAck;
    std::lock_guard<std::mutex> lock(m_sendEventMutex);
    m_sendingEventQueue.push(ev);
}

// Envoit un evenement de communication pour indiquer a l'envoi qu'on a recu un NAK
// L'evenement contiendra l'adresse d'ou provient l'information et le numero du NAK
void LinkLayer::notifyNAK(const Frame& frame)
{
    Event ev = Event::Invalid();
    ev.Type = EventType::NAK_RECEIVED;
    ev.Number = frame.Ack;
    ev.Address = frame.Source;
    std::lock_guard<std::mutex> lock(m_sendEventMutex);
    m_sendingEventQueue.push(ev);
}

// Envoit un evenement de communication pour indiquer au recepteur qu'on a atteint un timeout pour un ACK
// L'evenement contiendra le numero du Timer qui est arrive a echeance et le numero de la trame associe au Timer
void LinkLayer::ackTimeout(size_t timerID, NumberSequence numberData)
{
    Event ev;
    ev.Type = EventType::ACK_TIMEOUT;
    ev.Number = numberData;
    ev.TimerID = timerID;
    std::lock_guard<std::mutex> guard(m_receiveEventMutex);
    m_receivingEventQueue.push(ev);
}

// Envoit un evenement de communication pour indiquer a l'envoi qu'on n'a aps recu de reponse a un envoit et qu'il faut reenvoyer la trame
// L'evenement contiendra le numero de la trame et le numero du Timer qui est arrive a echeance
void LinkLayer::transmissionTimeout(size_t timerID, NumberSequence numberData)
{
    Event ev;
    ev.Type = EventType::SEND_TIMEOUT;
    ev.Number = numberData;
    ev.TimerID = timerID;
    std::lock_guard<std::mutex> guard(m_sendEventMutex);
    m_sendingEventQueue.push(ev);
}


// Demarre un nouveau Timer d'attente pour l'envoi a nouveau d'une trame
// La methode retourne le numero du Timer qui vient d'etre demarre. Cette valeur doit etre garder pour pouvoir retrouver quel evenement y sera associe lorsque
// le timer arrivera a echeance
size_t LinkLayer::startTimeoutTimer(NumberSequence numberData)
{
    return m_timers->addTimer(m_transmissionTimeout, std::bind(&LinkLayer::transmissionTimeout, this, std::placeholders::_1, std::placeholders::_2), numberData);
}

// Demarre un nouveau Timer pour l'envoi d'un ACK, pour garantir un niveau de service minimal dans une communication unidirectionnelle
// Retourne le numero du Timer qui vient d'etre demarre. La methode prend en parametre le numero actuel du Timer de ACK afin de le redemarrer s'il existe encore
size_t LinkLayer::startAckTimer(size_t existingTimerID, NumberSequence ackNumber)
{
    if (!m_timers->restartTimer(existingTimerID, ackNumber))
    {
        return m_timers->addTimer(m_ackTimeout, std::bind(&LinkLayer::ackTimeout, this, std::placeholders::_1, std::placeholders::_2), ackNumber);
    }
    return existingTimerID;
}

// Envoit un evenement de communication pour indiquer a la fonction de reception qu'une ACK vient d'etre envoyer (en piggybacking) et 
// qu'on n'a pas besoin d'envoyer le ACK en attente
void LinkLayer::notifyStopAckTimers(const MACAddress& to)
{
    Event ev;
    ev.Type = EventType::STOP_ACK_TIMER_REQUEST;
    ev.Address = to;
    std::lock_guard<std::mutex> guard(m_receiveEventMutex);
    m_receivingEventQueue.push(ev);
}

// Arrete le Timer de ACK avec le TimerID specifie
void LinkLayer::stopAckTimer(size_t timerID)
{
    m_timers->removeTimer(timerID);
}

// Indique s'il y a assez de place dans le buffer de reception pour recevoir des donnees de la couche physique
bool LinkLayer::canReceiveDataFromPhysicalLayer(const Frame& data) const
{
    return m_receivingQueue.canWrite<Frame>(data);
}

// Recoit des donnees de la couche physique
void LinkLayer::receiveData(Frame data)
{
    // Si la couche est pleine, la trame est perdue. Elle devra etre envoye a nouveau par l'emetteur
    if (canReceiveDataFromPhysicalLayer(data))
    {
        // Est-ce que la trame re�ue est pour nous?
        if (data.Destination == m_address || data.Destination.isMulticast())
        {
            m_receivingQueue.push(data);
        }
    }
}

// Fonction qui retourne l'adresse MAC du destinataire d'un packet particulier de la couche Reseau.
// Dans la realite, cette fonction ferait un lookup dans une table a partir des adresses IP pour recupere les addresse MAC.
// Ici, on utilise directement seulement les adresse MAC.
MACAddress LinkLayer::arp(const Packet& packet) const
{
    return packet.Destination;
}

// Fonction qui fait l'envoi des trames et qui gere la fenetre d'envoi
void LinkLayer::senderCallback()
{
    Frame outBuf[NB_BUFS];

    /*
    Ce sleep est nécéssaire pour corriger un problème avec le simulateur. Lorsque le fichier From_MAC1_to_MAC2... est
    créé, c'est comme si les threads perdent leur synchronisation. Donc, on se retrouve prit dans une boucle de timeout 
    et le fichier n'est pas complet. Si on sleep pour une seconde avant de commencer le traitement, le problème 
    disparait. C'est un peu un hack, mais ca marche so...
    */
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    while (m_executeSending)
    {
        Logger log(std::cout);
        // On recupere le dernier event
        Event sendEvent = getNextSendingEvent();

        switch (sendEvent.Type)
        {
            // Pas d'�v�nements, proceed as normal
            case EventType::INVALID:
            {
                // Est-ce qu'on doit envoyer des donnees
                if (m_driver->getNetworkLayer().dataReady())
                {
                    // La fenetre est pleine, on ne fait rien
                    if (m_bufferSize >= NB_BUFS) break;

                    m_bufferSize++;

                    Packet packet = m_driver->getNetworkLayer().getNextData();
                    Frame frame;
                    frame.Ack = (m_trameAttendue + m_maximumSequence) % (m_maximumSequence + 1);
                    frame.Destination = arp(packet);
                    frame.Source = m_address;
                    frame.NumberSeq = m_prochaineTrameAEnvoyer;
                    frame.Data = Buffering::pack<Packet>(packet);
                    frame.Size = (uint16_t)frame.Data.size();

                    // Ajout de la trame au buffer
                    outBuf[frame.NumberSeq % NB_BUFS] = frame;

                    // On envoit la trame. Si la trame n'est pas envoyee,
                    // c'est qu'on veut arreter le simulateur
                    if (!sendFrame(frame))
                    {
                        return;
                    }

                    // Arret du timer pour l'envoi du ACK en piggybacking
                    notifyStopAckTimers(frame.Destination);

                    // On cree un nouveau timerID pour le renvoi
                    size_t timerID = startTimeoutTimer(frame.NumberSeq);
                    
                    // On insere l'element dans le buffer
                    m_sendTimers[timerID] = frame;

                    // On ajuste le NextID
                    m_prochaineTrameAEnvoyer = (m_prochaineTrameAEnvoyer + 1) % (m_maximumSequence + 1);
                }
                break;
            }
            // Delai ecoule pour la reception de la trame
            case EventType::SEND_TIMEOUT:
            {
                log << "SEND_TIMEOUT: " << sendEvent.TimerID << " event reached for frame: " << sendEvent.Number << std::endl;

                // On arrete tout les timers de ACK
                for (auto it = m_sendTimers.begin(); it != m_sendTimers.end(); ++it)
                {
                    // On a un timer inferieur a celui qu'on doit arreter
                    if (it->first < sendEvent.TimerID)
                    {
                        // On arrete tout les timers
                        stopAckTimer(it->first);
                        // On efface la donnee du buffer
                        m_ackTimers.erase(it);
                    }
                }
                // On recupere la trame
                Frame frame = m_sendTimers[sendEvent.TimerID];

                // Envoi de la trame
                if (!sendFrame(frame))
                {
                    return;
                }

                size_t timerID = startTimeoutTimer(sendEvent.Number);

                m_sendTimers[timerID] = frame;

                break;
            }
            // On a recue la bonne trame et on desire envoyer un ACK a la source
            case EventType::SEND_ACK_REQUEST:
            {
                Frame frame;
                frame.Destination = sendEvent.Address;
                frame.Source = m_address;
                frame.NumberSeq = sendEvent.Number;
                frame.Size = FrameType::ACK;

                // On envoit le ACK a l'autre machine
                if (!sendFrame(frame))
                {
                    return;
                }
                break;
            }
            case EventType::SEND_NAK_REQUEST:
            {
                Frame frame;
                frame.Destination = sendEvent.Address;
                frame.Source = m_address;
                frame.NumberSeq = sendEvent.Number;
                frame.Size = FrameType::NAK;

                // On envoit le ACK a l'autre machine
                if (!sendFrame(frame))
                {
                    return;
                }
                break;
            }
            default:
                log << "default" << std::endl;
                break;
        }
        
    }
}

// Fonction qui s'occupe de la reception des trames
void LinkLayer::receiverCallback()
{ 
    Frame inBuf[NB_BUFS];
    bool arrive[NB_BUFS];

    for (int i = 0; i < NB_BUFS; i++){
        arrive[i] = false;
    }

    /*
    Ce sleep est nécéssaire pour corriger un problème avec le simulateur. Lorsque le fichier From_MAC1_to_MAC2... est
    créé, c'est comme si les threads perdent leur synchronisation. Donc, on se retrouve prit dans une boucle de timeout
    et le fichier n'est pas complet. Si on sleep pour une seconde avant de commencer le traitement, le problème
    disparait. C'est un peu un hack, mais ca marche so...
    */
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    while (m_executeReceiving)
    {
        Logger log(std::cout);
        Event recEvent = getNextReceivingEvent();

        switch (recEvent.Type)
        {
            // Aucun evenement particulier
            case EventType::INVALID:
            {
                if (m_receivingQueue.canRead<Frame>())
                {
                    Frame frame = m_receivingQueue.pop<Frame>();

                    // Trame de donnees
                    if (frame.Size != FrameType::ACK || frame.Size != FrameType::NAK)
                    {
                        if ((frame.NumberSeq != m_trameAttendue) && no_nak)
                        {
                            // On a recu une trame dans le mauvais ordre, on a forcement une erreur de communication
                            sendNak(frame.Source, m_trameAttendue);
                        }
                        else
                        {
                            size_t timerID = startAckTimer(0, frame.NumberSeq);
                            m_ackTimers[timerID] = frame;
                        }

                        if (between(frame.NumberSeq, m_trameAttendue, m_tropLoin) && (arrive[frame.NumberSeq % NB_BUFS] == false))
                        {
                            arrive[frame.NumberSeq % NB_BUFS] = true;
                            inBuf[frame.NumberSeq % NB_BUFS] = frame;

                            while (arrive[m_trameAttendue % NB_BUFS])
                            {
                                m_driver->getNetworkLayer().receiveData(Buffering::unpack<Packet>(inBuf[m_trameAttendue % NB_BUFS].Data));
                                no_nak = true;
                                arrive[m_trameAttendue % NB_BUFS] = false;
                                m_trameAttendue = (m_trameAttendue + 1) % (m_maximumSequence + 1);
                                m_tropLoin = (m_tropLoin + 1) % (m_maximumSequence + 1);

                                size_t timerID = startAckTimer(0, m_ackAttendu);
                                m_ackTimers[timerID] = frame;
                            }
                        }
                    }
                    if ((frame.Size == FrameType::NAK) && between((frame.Ack + 1) % (m_maximumSequence + 1), m_ackAttendu, m_prochaineTrameAEnvoyer))
                    {
                        notifyNAK(frame);
                    }

                    while (between(frame.Ack, m_ackAttendu, m_prochaineTrameAEnvoyer))
                    {
                        m_bufferSize--;
                        stopAckTimer(m_ackAttendu);
                        m_ackAttendu = (m_ackAttendu + 1) % (m_maximumSequence + 1);
                    }
                    break;
                }
                break;
            }
            case EventType::STOP_ACK_TIMER_REQUEST:
            {
                // On arrete tout les timers de ACK
                for (auto it = m_ackTimers.begin(); it != m_ackTimers.end(); ++it)
                {
                    // On a un timer inferieur a celui qu'on doit arreter
                    if (it->first <= recEvent.TimerID){
                        // On arrete tout les timers
                        stopAckTimer(it->first);
                        // On efface la donnee du buffer
                        m_ackTimers.erase(it);
                    }
                }
                break;
            }
            case EventType::ACK_TIMEOUT:
            {
                // Timer pour le ACK ecoule, on doit envoyer une trame ACK sans piggybacking
                sendAck(recEvent.Address, recEvent.Number);
                break;
            }

            default:
            log << "default" << std::endl;
            break;
        }   
    }
}
