#include "LinkLayerLow.h"

#include "LinkLayer.h"
#include "../NetworkDriver.h"
#include "../../../DataStructures/DataBuffer.h"
#include "../../../General/Configuration.h"
#include "../../../General/Logger.h"

#include <iostream>

std::unique_ptr<DataEncoderDecoder> DataEncoderDecoder::CreateEncoderDecoder(const Configuration& config)
{
    int encoderDecoderConfig = config.get(Configuration::LINK_LAYER_LOW_DATA_ENCODER_DECODER);
    if (encoderDecoderConfig == 1)
    {
        return std::make_unique<HammingDataEncoderDecoder>();
    }
    else if (encoderDecoderConfig == 2)
    {
        return std::make_unique<CRCDataEncoderDecoder>();
    }
    else
    {
        return std::make_unique<PassthroughDataEncoderDecoder>();
    }
}


DynamicDataBuffer PassthroughDataEncoderDecoder::encode(const DynamicDataBuffer& data) const
{
    return data;
}

std::pair<bool, DynamicDataBuffer> PassthroughDataEncoderDecoder::decode(const DynamicDataBuffer& data) const
{
    return std::pair<bool, DynamicDataBuffer>(true, data);
}


//===================================================================
// Hamming Encoder decoder implementation
//===================================================================
HammingDataEncoderDecoder::HammingDataEncoderDecoder()
{
	// À faire TP
}

HammingDataEncoderDecoder::~HammingDataEncoderDecoder()
{
	// À faire TP
}

DynamicDataBuffer HammingDataEncoderDecoder::encode(const DynamicDataBuffer& data) const
{
    int r_size = 0, pair;    // r_size = nombre de bits de redondance

    int m_size = data.size();  //m_size = nombre de bits du message à coder

    // Nous cherchons le nombre de bits de redondance
    while (pow(2, r_size) < m_size + r_size + 1) {
        r_size++;
    }


    // int hamming[size + r], j = 0, k = 1;
    int hamming_code[32], j = 0, k = 1;


    // Nous cherchons les positions des bits de redondance
    for (int i = 1; i <= m_size + r_size; i++) {
        if (i == pow(2, j)) {
            hamming_code[i] = -1;    //-1 est la valeur initiale des bits de redondance
            j++;
        }
        else {
            hamming_code[i] = data[k - 1];
            k++;
        }
    }

    k = 0;
    int mini, maxi, x = 0;

    // Nous trouvons par la suite la parité des bit
    for (int i = 1; i <= m_size + r_size; i = pow(2, k)) {
        k++;
        pair = 0;
        j = i;
        x = i;
        mini = 1;
        maxi = i;
        while (j <= m_size + r_size) {
            for (x = j; maxi >= mini && x <= m_size + r_size; mini++, x++) {
                if (hamming_code[x] == 1)
                    pair = pair + 1;;
            }
            j = x + i;
            mini = 1;
        }

        // Vérification de la parité
        if (pair % 2 == 0) {
            hamming_code[i] = 0;
        }
        else {
            hamming_code[i] = 1;
        }
    }

    // Nous créons le buffer qu'on va retourner
    uint32_t new_size = m_size + r_size;
    DynamicDataBuffer code(new_size);

    // Remplissage de buffer
    for (int i = 0; i < code.size(); i++)
    {
        code[i] = hamming_code[i + 1];
    }
    return code;
}

std::pair<bool, DynamicDataBuffer> HammingDataEncoderDecoder::decode(const DynamicDataBuffer& data) const
{
    int size = data.size();  // size = nombre de bits du code ruçu
    int code[32];
    for (int i = 1; i <= size; ++i)
        code[i] = data[i];

    // Nous cherchons le nombre de bits de redondance
    int r_size = 0;
    for (int i = 1; i <= size; i++)
    {
        if (pow(2, r_size) == i)
            r_size++;
    }

    int d = 0, ec = 0;

    // Nous calculons les bits de parité et nous comparons afin de détecter les erreurs
    // NB: Cette méthode implémenté permet de façon efficace de corriger un seul bit erroné
    //     Mais elle ne permet pas de corriger une trame avec plusieurs bit erronés
    int mini = 1, maxi = 0, s, k, pair, err[10] = { 0 };
    for (int i = 1; i <= size; i = pow(2, d))
    {
        ++d;
        pair = 0;
        s = i;
        k = i;
        mini = 1;
        maxi = i;

        // Nous cherchons le bit de redendance qui est supposé reçu
        for (s; s <= size;)
        {
            for (k = s; maxi >= mini && k <= size; ++mini, ++k)
            {
                if (code[k] == 1)
                    pair++;
            }
            s = k + i;
            mini = 1;
        }

        // Si c'est la meme parité il n'y a pas d'erreur, si non on marque la position de l'erreur
        if (pair % 2 == 0) // Même parité
        {
            err[ec] = 0;
            ec++;
        }
        else
        {
            err[ec] = 1;
            ec++;
        }
    }

    // Nous vérifions ici si nous avons détecté une erreur ou pas
    int flag = 1;
    for (int i = r_size - 1; i >= 0; i--)
    {
        if (err[i] == 1)
        {
            flag = 0;
            break;
        }
    }

    // Dans le cas de la présence d'une erreur, nous retournons un booléen False
    // avec le code corrigé
    if (flag == 0)
    {
        int position = 0;
        for (int i = r_size - 1; i >= 0; i--)
        {
            if (err[i] == 1)
                position += pow(2, i);
        }
        std::cout << "\nUne erreur a été detecté à la position: " << position;
        code[position] = !code[position];

        // Nous créons le buffer qu'on va retourner
        uint32_t new_size = size;
        DynamicDataBuffer result(new_size);

        // Remplissage de buffer
        for (int i = 0; i < result.size(); i++)
        {
            result[i] = code[i + 1];
        }
        return std::pair<bool, DynamicDataBuffer>(false, result);
    }

    // Dans le cas de l'abscence d'erreur, nous retournons un booléen True
    // avec le code reçu
    else
        return std::pair<bool, DynamicDataBuffer>(true, data);
}



//===================================================================
// CRC Encoder decoder implementation
//===================================================================
CRCDataEncoderDecoder::CRCDataEncoderDecoder()
{
}

CRCDataEncoderDecoder::~CRCDataEncoderDecoder()
{
}

DynamicDataBuffer CRCDataEncoderDecoder::encode(const DynamicDataBuffer& data) const
{
    return data;
}

std::pair<bool, DynamicDataBuffer> CRCDataEncoderDecoder::decode(const DynamicDataBuffer& data) const
{
    return std::pair<bool, DynamicDataBuffer>(true, data);
}


//===================================================================
// Network Driver Physical layer implementation
//===================================================================
LinkLayerLow::LinkLayerLow(NetworkDriver* driver, const Configuration& config)
    : m_driver(driver)
    , m_sendingBuffer(config.get(Configuration::LINK_LAYER_LOW_SENDING_BUFFER_SIZE))
    , m_receivingBuffer(config.get(Configuration::LINK_LAYER_LOW_RECEIVING_BUFFER_SIZE))
    , m_stopReceiving(true)
    , m_stopSending(true)
{
    m_encoderDecoder = DataEncoderDecoder::CreateEncoderDecoder(config);
}

LinkLayerLow::~LinkLayerLow()
{
    stop();
    m_driver = nullptr;
}

void LinkLayerLow::start()
{
    stop();

    start_receiving();
    start_sending();
}

void LinkLayerLow::stop()
{
    stop_receiving();
    stop_sending();
}

bool LinkLayerLow::dataReceived() const
{
    return m_receivingBuffer.canRead<DynamicDataBuffer>();
}

DynamicDataBuffer LinkLayerLow::encode(const DynamicDataBuffer& data) const
{
    return m_encoderDecoder->encode(data);
}

std::pair<bool, DynamicDataBuffer> LinkLayerLow::decode(const DynamicDataBuffer& data) const
{
    return m_encoderDecoder->decode(data);
}

void LinkLayerLow::start_receiving()
{
    m_stopReceiving = false;
    m_receivingThread = std::thread(&LinkLayerLow::receiving, this);
}

void LinkLayerLow::stop_receiving()
{
    m_stopReceiving = true;
    if (m_receivingThread.joinable())
    {
        m_receivingThread.join();
    }
}

void LinkLayerLow::start_sending()
{
    m_stopSending = false;
    m_sendingThread = std::thread(&LinkLayerLow::sending, this);
}

void LinkLayerLow::stop_sending()
{
    m_stopSending = true;
    if (m_sendingThread.joinable())
    {
        m_sendingThread.join();
    }
}


void LinkLayerLow::receiving()
{
    while (!m_stopReceiving)
    {
        if (dataReceived())
        {
            DynamicDataBuffer data = m_receivingBuffer.pop<DynamicDataBuffer>();
            std::pair<bool, DynamicDataBuffer> dataBuffer = decode(data);
            if (dataBuffer.first) // Les donnees recues sont correctes et peuvent etre utilisees
            {
                Frame frame = Buffering::unpack<Frame>(dataBuffer.second);
                m_driver->getLinkLayer().receiveData(frame);
            }
            else
            {
                // Les donnees recues sont corrompues et doivent etre delaissees
                Logger log(std::cout);
                log << m_driver->getMACAddress() << " : Corrupted data received" << std::endl;
            }
        }
    }
}

void LinkLayerLow::sending()
{
    while (!m_stopSending)
    {
        if (m_driver->getLinkLayer().dataReady())
        {
            Frame dataFrame = m_driver->getLinkLayer().getNextData();
            DynamicDataBuffer buffer = encode(Buffering::pack<Frame>(dataFrame));
            sendData(buffer);
        }
    }
}

void LinkLayerLow::receiveData(const DynamicDataBuffer& data)
{
    // Si le buffer est plein, on fait juste oublier les octets recus du cable
    // Sinon, on ajoute les octets au buffer
    if (m_receivingBuffer.canWrite<DynamicDataBuffer>(data))
    {
        m_receivingBuffer.push(data);
    }
    else
    {
        Logger log(std::cout);
        log << m_driver->getMACAddress() << " : Physical reception buffer full... data discarded" << std::endl;
    }
}

void LinkLayerLow::sendData(DynamicDataBuffer data)
{
    // Envoit une suite d'octet sur le cable connecte
    m_driver->sendToCard(data);
}