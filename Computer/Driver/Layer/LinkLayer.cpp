#include "LinkLayer.h"
#include "../NetworkDriver.h"

#include "../../../General/Configuration.h"
#include "../../../General/Logger.h"

#include <iostream>
#include <functional>
#include <map>


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
    m_maximumSequence = m_maximumBufferedFrameCount * 2 - 1;
    m_ackTimeout = m_transmissionTimeout / 4;
    m_timers = std::make_unique<Timer>();
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
    // �� faire TP
    // Remplacer le code suivant qui ne fait qu'envoyer les trames dans l'ordre re�u sans validation
    // afin d'ex�cuter le protocole � fen�tre demand� dans l'�nonc�.
    NumberSequence nextID = 0;
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
                    Packet packet = m_driver->getNetworkLayer().getNextData();
                    Frame frame;
                    frame.Destination = arp(packet);
                    frame.Source = m_address;
                    frame.NumberSeq = nextID++;
                    frame.Data = Buffering::pack<Packet>(packet);
                    frame.Size = (uint16_t)frame.Data.size();

                    // On envoit la trame. Si la trame n'est pas envoyee,
                    // c'est qu'on veut arreter le simulateur
                    if (!sendFrame(frame))
                    {
                        return;
                    }
                    // On cree un nouveau timerID pour le renvoi
                    size_t timerID = startTimeoutTimer(frame.NumberSeq);
                    // On insere l'element dans le buffer
                    sendBuffer.insert(std::make_pair(timerID, frame));
                }
            }
            // Delai ecoule pour la reception de la trame
            case EventType::SEND_TIMEOUT:
            {
                // log << "SEND_TIMEOUT event reached for frame: " << sendEvent.Number << std::endl;
                               
                // On cherche le frame associe a notre timerID
                auto it = sendBuffer.find(sendEvent.TimerID);

                if (it != sendBuffer.end())
                {
                    // Le timerID est toujours actif, on renvoie la trame
                    Frame frame = it->second;
                    if (!sendFrame(frame))
                    {
                        return;
                    }
                    log << "Sending frame " << frame.NumberSeq << " again." << std::endl;
                    // On cree un nouveau timerID pour le renvoi
                    size_t timerID = startTimeoutTimer(frame.NumberSeq);

                    // On remplace l'element dans le buffer d'envoie
                    sendBuffer.erase(it);
                    sendBuffer.insert(std::make_pair(timerID, frame));
                }
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

            default:
                log << "default" << std::endl;
                break;
        }
        
    }
}

// Fonction qui s'occupe de la reception des trames
void LinkLayer::receiverCallback()
{
    // �� faire TP
    // Remplacer le code suivant qui ne fait que recevoir les trames dans l'ordre re�u sans validation
    // afin d'ex�cuter le protocole � fen�tre demand� dans l'�nonc�.
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

                    if (frame.Size == FrameType::ACK)
                    {
                        log << "ACK recu, on efface la trame " << frame.NumberSeq << " du buffer d'envoie" << std::endl;
                        
                        // On cherche le timerID associe a notre trame et on le stoppe
                        for (auto it = sendBuffer.begin(); it != sendBuffer.end(); ++it)
                        {
                            Frame bufferFrame = it->second;

                            if (frame.NumberSeq == bufferFrame.NumberSeq)
                            {
                                // Le timerID est toujours actif
                                stopAckTimer(it->first);
                                removeFrameFromSendBuffer(it->second);
                                break;
                            }
                        }
                        // Ne devrait jamais se produire, redondant comme break
                        break;
                    }
                    else if (frame.Size == FrameType::NAK)
                    {
                        log << "Nak in receiving" << std::endl;
                        // Traitement du NAK
                    }
                    else
                    {
                        log << "Normal frame received" << std::endl;
                        // Si on a recu la bonne trame on  traite, sinon on ignore
                        if (frame.NumberSeq == m_nSeqExpected)
                        {
                            log << "Trame " << frame.NumberSeq << " recue, on fait le traitement." << std::endl;
                            // On a recu la bonne trame, on incremente la trame attendue
                            m_nSeqExpected++;

                            // On envoie les donnees a la couche reseau
                            m_driver->getNetworkLayer().receiveData(Buffering::unpack<Packet>(frame.Data));
                        }
                        // On fait une demande d'envoie de ACK pour la trame recue
                        sendAck(frame.Source, frame.NumberSeq);
                        break;
                    }
                }
                break;
            }

            default: 
                log << "default" << std::endl;
                break;
        }   
    }
}

void LinkLayer::removeFrameFromSendBuffer(const Frame& frame)
{
    // Parcours de la liste des trames envoyee
    for (auto iter = sendBuffer.begin(); iter != sendBuffer.end(); ++iter)
    {
        // On extrait les donnees du buffer
        Frame bufferFrame = iter->second;

        if (bufferFrame.NumberSeq == frame.NumberSeq)
        {
            sendBuffer.erase(iter);
            return;            
        }
    }
}

void LinkLayer::processDataFrame(const Frame& frame)
{
    Logger log(std::cout);
    // Si on a recu la bonne trame on  traite, sinon on ignore
    if (frame.NumberSeq == m_nSeqExpected)
    {
        log << "Trame " << frame.NumberSeq << " recue, on fait le traitement." << std::endl;
        // On a recu la bonne trame, on incremente la trame attendue
        m_nSeqExpected++;

        // On fait une demande d'envoie de ACK pour la trame recue
        sendAck(frame.Source, frame.NumberSeq);

        // On envoie les donnees a la couche reseau
        m_driver->getNetworkLayer().receiveData(Buffering::unpack<Packet>(frame.Data));
        return;
    }
    log << "Trame " << frame.NumberSeq << " ignoree, on attendait " << m_nSeqExpected << std::endl;
}


// case EventType::ACK_RECEIVED:
// // Not yet implemented
// break;

// // On est dans l'attente d'un ACK pour le paquet
// case EventType::SEND_ACK_REQUEST:

// Event nextEvent;

// // Attente active pour un ACK recu ou le timer a expire
// while (nextEvent.Type != EventType::ACK_RECEIVED || nextEvent.Type != EventType::SEND_TIMEOUT)
// {
//     Event nextEvent = getNextSendingEvent();
// }

// // Le timer a expire, on renvoit la trame et on repart le timer pour le ACK
// if (nextEvent.Type == EventType::SEND_TIMEOUT)
// {
//     for (auto frame : inBuffer)
//     {
//         // On doit renvoyer cet evenement
//         if (frame.NumberSeq == nextEvent.Number)
//         {
//             if (!sendFrame(frame))
//             {
//                 return;
//             }
//         }
//     }

// }

// case EventType::NAK_RECEIVED:
// // Not yet implemented
// break;
