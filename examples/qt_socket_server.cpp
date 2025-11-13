// SPDX-License-Identifier: MIT
//
// A minimal Qt-based TCP server example that listens for length-prefixed
// binary payloads, prints them, and sends a short acknowledgement back to the
// client.  The focus is clarity over completeness so that a Qt developer who
// is new to the POSIX sockets API can still use familiar Qt network classes.
//
// Build instructions (Qt 6):
//   mkdir build && cd build
//   qmake -project "QT += core network" "CONFIG += console" -o qt_socket_server.pro ../qt_socket_server.cpp
//   qmake qt_socket_server.pro
//   make
//   ./qt_socket_server
//
// When using CMake:
//   cmake -S .. -B build -DQT_DEFAULT_MAJOR_VERSION=6
//   cmake --build build
//
// The server expects each client message to begin with a 32-bit unsigned
// length (network byte order) followed by the opaque payload bytes.  This keeps
// the example interoperable with the standalone POSIX client/server samples.
//
// Note: Qt sockets are asynchronous.  The server runs inside the Qt event loop
// and reacts to incoming data via the readyRead() signal.

#include <QAbstractSocket>
#include <QByteArray>
#include <QCoreApplication>
#include <QDataStream>
#include <QDateTime>
#include <QDebug>
#include <QHostAddress>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QString>
#include <QVariant>

#include <cstdlib>

class MessageServer final : public QObject {
    Q_OBJECT

public:
    explicit MessageServer(quint16 port, QObject *parent = nullptr)
        : QObject(parent), m_port(port) {
        // When a new TCP client connects, handle it in onNewConnection().
        connect(&m_server, &QTcpServer::newConnection, this, &MessageServer::onNewConnection);
    }

    bool start() {
        if (!m_server.listen(QHostAddress::Any, m_port)) {
            qCritical() << "Failed to listen on port" << m_port << ':' << m_server.errorString();
            return false;
        }

        qInfo() << "Qt message server listening on port" << m_port;
        return true;
    }

private slots:
    void onNewConnection() {
        while (QTcpSocket *socket = m_server.nextPendingConnection()) {
            qInfo() << "Client connected from" << socket->peerAddress().toString() << ':' << socket->peerPort();

            // Track partial reads by storing the expected payload size as dynamic property.
            socket->setProperty("expectedSize", QVariant::fromValue<quint32>(0));

            connect(socket, &QTcpSocket::readyRead, this, &MessageServer::onReadyRead);
            connect(socket, &QTcpSocket::disconnected, this, &MessageServer::onClientDisconnected);
            connect(socket, &QTcpSocket::errorOccurred, this, &MessageServer::onSocketError);
        }
    }

    void onReadyRead() {
        auto *socket = qobject_cast<QTcpSocket *>(sender());
        if (!socket) {
            return;
        }

        QDataStream stream(socket);
        stream.setByteOrder(QDataStream::BigEndian);  // Match POSIX example.

        quint32 expectedSize = socket->property("expectedSize").toUInt();

        // If we have not yet read the payload length, try to do so now.
        if (expectedSize == 0) {
            if (socket->bytesAvailable() < static_cast<int>(sizeof(quint32))) {
                return;  // Wait for more data.
            }

            stream >> expectedSize;
            socket->setProperty("expectedSize", expectedSize);
        }

        if (socket->bytesAvailable() < expectedSize) {
            return;  // Not enough payload data yet.
        }

        QByteArray payload(expectedSize, Qt::Uninitialized);
        const qint64 readBytes = stream.readRawData(payload.data(), expectedSize);
        if (readBytes != expectedSize) {
            qWarning() << "Short read: expected" << expectedSize << "bytes but got" << readBytes;
            return;
        }

        socket->setProperty("expectedSize", QVariant::fromValue<quint32>(0));

        qInfo() << "Received" << expectedSize << "bytes from" << socket->peerAddress().toString();

        // Echo the payload to stdout in hex for demonstration purposes.
        qInfo().noquote() << "Payload (hex):" << payload.toHex(' ');

        sendAcknowledgement(socket, payload.size());
    }

    void onClientDisconnected() {
        auto *socket = qobject_cast<QTcpSocket *>(sender());
        if (!socket) {
            return;
        }

        qInfo() << "Client disconnected:" << socket->peerAddress().toString();
        socket->deleteLater();
    }

    void onSocketError(QAbstractSocket::SocketError error) {
        auto *socket = qobject_cast<QTcpSocket *>(sender());
        if (!socket) {
            return;
        }

        qWarning() << "Socket error from" << socket->peerAddress().toString() << ':' << error << '-' << socket->errorString();
    }

private:
    void sendAcknowledgement(QTcpSocket *socket, qsizetype payloadSize) {
        const QString ackText = QStringLiteral("[%1] ACK: Received %2 bytes")
                                     .arg(QDateTime::currentDateTime().toString(Qt::ISODate))
                                     .arg(payloadSize);

        QByteArray ackPayload = ackText.toUtf8();

        QDataStream out(socket);
        out.setByteOrder(QDataStream::BigEndian);
        out << static_cast<quint32>(ackPayload.size());
        out.writeRawData(ackPayload.constData(), ackPayload.size());

        if (!socket->waitForBytesWritten(5000)) {
            qWarning() << "Failed to send acknowledgement:" << socket->errorString();
        }
    }

    QTcpServer m_server;
    quint16 m_port;
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);

    const quint16 port = 45454;  // Arbitrary demo port.
    MessageServer server(port);
    if (!server.start()) {
        return EXIT_FAILURE;
    }

    return app.exec();
}

#include "qt_socket_server.moc"

