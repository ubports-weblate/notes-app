#ifndef CORE_H
#define CORE_H

#include <QObject>

class Core: public QObject
{
    Q_OBJECT
public:
    Core(QObject *parent = 0);

    void process(const QByteArray &pushNotification);


signals:
    void finished(int ret);

private slots:
    void connectedChanged();
    void oaRequestFinished(const QVariantMap &reply);

    void notesLoaded();
};

#endif