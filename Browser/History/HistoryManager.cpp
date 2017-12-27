#include "HistoryManager.h"

#include <QBuffer>
#include <QDateTime>
#include <QIcon>
#include <QImage>
#include <QRegExp>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUrl>
#include <QDebug>

HistoryManager::HistoryManager(bool firstRun, const QString &databaseFile, QObject *parent) :
    QWebHistoryInterface(parent),
    DatabaseWorker(databaseFile, "HistoryDB"),
    m_lastVisitID(0),
    m_historyItems(),
    m_recentItems(),
    m_queryHistoryItem(nullptr),
    m_queryVisit(nullptr)
{
    // Setup structure if first run
    if (firstRun)
        setup();

    load();

    QWebHistoryInterface::setDefaultInterface(this);
}

HistoryManager::~HistoryManager()
{
    //save();
    if (m_queryHistoryItem != nullptr)
    {
        delete m_queryHistoryItem;
        delete m_queryVisit;
    }
}

void HistoryManager::addHistoryEntry(const QString &url)
{
    QString lowerUrl = url.toLower();

    // Check if qrc:/ resource, and if so, ignore
    if (lowerUrl.startsWith("qrc:"))
        return;

    QDateTime visitTime = QDateTime::currentDateTime();

    auto it = m_historyItems.find(lowerUrl);
    if (it != m_historyItems.end())
    {
        it->Visits.prepend(visitTime);
        m_recentItems.prepend(*it);
        while (m_recentItems.size() > 15)
            m_recentItems.removeLast();
        if (!it->Title.isEmpty())
        {
            saveVisit(*it, visitTime);
            emit pageVisited(lowerUrl, it->Title);
        }
        return;
    }

    WebHistoryItem item;
    item.URL = QUrl::fromUserInput(lowerUrl);
    item.VisitID = ++m_lastVisitID;
    item.Visits.prepend(visitTime);
    m_historyItems.insert(lowerUrl, item);
    m_recentItems.prepend(item);
    while (m_recentItems.size() > 15)
        m_recentItems.removeLast();
    // don't emit pageLoaded until title is set
}

void HistoryManager::clearAllHistory()
{
    QSqlQuery query(m_database);
    if (!query.exec("DELETE FROM History"))
    {
        qDebug() << "[Error]: In HistoryManager::clearAllHistory - Unable to clear History table. Message: "
                 << query.lastError().text();
    }
    if (!query.exec("DELETE FROM Visits"))
    {
        qDebug() << "[Error]: In HistoryManager::clearAllHistory - Unable to clear Visits table. Message: "
                 << query.lastError().text();
    }

    m_recentItems.clear();
    m_historyItems.clear();
}

void HistoryManager::clearHistoryFrom(const QDateTime &start)
{
    // Perform database query and reload data
    QSqlQuery query(m_database);
    query.prepare("DELETE FROM Visits WHERE Date > (:date)");
    query.bindValue(":date", start.toMSecsSinceEpoch());
    if (!query.exec())
    {
        qDebug() << "[Error]: In HistoryManager::clearHistoryFrom - Unable to clear history. Message: "
                 << query.lastError().text();
        return;
    }

    m_recentItems.clear();
    m_historyItems.clear();
    load();
}

bool HistoryManager::historyContains(const QString &url) const
{
    return (m_historyItems.find(url.toLower()) != m_historyItems.end());
}

void HistoryManager::setTitleForURL(const QString &url, const QString &title)
{
    QString lowerUrl = url.toLower();
    auto it = m_historyItems.find(lowerUrl);
    if (it != m_historyItems.end())
    {
        if (it->Title.isEmpty())
        {
            it->Title = title;

            int recentVisitIdx = m_recentItems.indexOf(*it);
            if (recentVisitIdx >= 0)
                m_recentItems[recentVisitIdx].Title = title;

            saveVisit(*it, it->Visits.front());
            emit pageVisited(lowerUrl, title);
        }
    }
}

QList<WebHistoryItem> HistoryManager::getHistoryFrom(const QDateTime &startDate) const
{
    QList<WebHistoryItem> items;

    if (!startDate.isValid())
        return items;

    // Get startDate in msec format
    qint64 startMSec = startDate.toMSecsSinceEpoch();

    // Prepare DB query (date is in MSecsSinceEpoch())
    QSqlQuery query(m_database);
    query.prepare("SELECT Date FROM Visits WHERE VisitID = (:id) AND Date >= (:date)");

    // Iterate through list of all visited URLs, searching the DB to confirm the date is between startDate and now
    for (const auto &it : m_historyItems)
    {
        WebHistoryItem item;
        item.Title = it.Title;
        item.URL = it.URL;
        item.VisitID = it.VisitID;
        item.Visits = it.Visits;

        // Check DB
        query.bindValue(":id", it.VisitID);
        query.bindValue(":date", startMSec);
        if (query.exec())
        {
            while (query.next())
            {
                item.Visits.append(QDateTime::fromMSecsSinceEpoch(query.value(0).toLongLong()));//dbVisit));
            }
        }
        items.append(item);
    }
    return items;
}

int HistoryManager::getTimesVisited(const QString &host)
{
    QSqlQuery query(m_database);
    query.prepare("SELECT COUNT(VisitID) FROM Visits WHERE VisitID = (:id)");
    int timesVisited = 0;
    for (const WebHistoryItem &item : m_historyItems)
    {
        if (host.endsWith(item.URL.host().remove(QRegExp("(http(s)?://)?(www.)"))))
        {
            query.bindValue(":id", item.VisitID);
            if (query.exec() && query.next())
                timesVisited += query.value(0).toInt();
        }
    }
    return timesVisited;
}

void HistoryManager::setup()
{
    QSqlQuery query(m_database);
    if (!query.exec("CREATE TABLE IF NOT EXISTS Visits(VisitID INTEGER NOT NULL, Date INTEGER NOT NULL, "
                    "PRIMARY KEY(VisitID, Date))"))
    {
        qDebug() << "[Error]: In HistoryManager::setup - unable to create visited table. Message: " << query.lastError().text();
        return;
    }
    if (!query.exec("CREATE TABLE IF NOT EXISTS History(URL TEXT PRIMARY KEY, Title TEXT, VisitID INTEGER NOT NULL, "
                    "FOREIGN KEY(VisitID) REFERENCES Visits(VisitID) ON DELETE CASCADE)"))
    {
        qDebug() << "[Error]: In HistoryManager::setup - unable to create history table. Message: " << query.lastError().text();
    }
}

void HistoryManager::load()
{
    // Only load data from the History table, will load specific visits if user requests full history
    QSqlQuery query(m_database);

    if (query.exec("SELECT URL, TITLE, VisitID FROM History ORDER BY VisitID ASC"))
    {
        QSqlRecord rec = query.record();
        int idUrl = rec.indexOf("URL");
        int idTitle = rec.indexOf("Title");
        int idVisit = rec.indexOf("VisitID");
        while (query.next())
        {
            WebHistoryItem item;
            item.URL = query.value(idUrl).toString();
            item.Title = query.value(idTitle).toString();
            item.VisitID = query.value(idVisit).toInt();

            m_lastVisitID = item.VisitID;
            m_historyItems.insert(item.URL.toString().toLower(), item);
        }
    }
    else
        qDebug() << "[Error]: In HistoryManager::load - Unable to fetch history items from the database. Message: "
                 << query.lastError().text();

    // Load most recent visits
    if (query.exec("SELECT Visits.Date, History.URL FROM Visits INNER JOIN History ON Visits.VisitID = History.VisitID ORDER BY Visits.Date DESC LIMIT 15"))
    {
        QSqlRecord rec = query.record();
        int idDate = rec.indexOf("Date");
        int idUrl = rec.indexOf("URL");
        while (query.next())
        {
            auto it = m_historyItems.find(query.value(idUrl).toString());
            if (it != m_historyItems.end())
            {
                QDateTime date = QDateTime::fromMSecsSinceEpoch(query.value(idDate).toULongLong());
                it->Visits.prepend(date);
                m_recentItems.prepend(*it);
            }
        }
    }
    else
        qDebug() << "Could not load visit date info. Message: " << query.lastError().text();
}

void HistoryManager::save()
{
    /*
    QSqlQuery query(m_database), queryVisits(m_database);
    query.prepare("INSERT OR REPLACE INTO History(URL, Title, VisitID) VALUES(:url, :title, :visitId)");
    queryVisits.prepare("INSERT OR REPLACE INTO Visits(VisitID, Date) VALUES(:visitId, :date)");

    for (auto it = m_historyItems.begin(); it != m_historyItems.end(); ++it)
    {
        const WebHistoryItem &item = it.value();

        // Skip item if title is blank - means the item was visited in private browsing mode
        if (item.Title.isEmpty())
            continue;

        query.bindValue(":url", it.key());
        query.bindValue(":title", item.Title);
        query.bindValue(":visitId", item.VisitID);

        if (!query.exec())
            qDebug() << "[Error]: In HistoryManager::save - unable to save history item to database. Message: " << query.lastError().text();

        // save the item's visits into the visits table
        for (auto visit : item.Visits)
        {
            queryVisits.bindValue(":visitId", item.VisitID);
            queryVisits.bindValue(":date", visit.toMSecsSinceEpoch());
            queryVisits.exec();
        }
    }
    */
}

void HistoryManager::saveVisit(const WebHistoryItem &item, const QDateTime &visitTime)
{
    if (m_queryHistoryItem == nullptr)
    {
        m_queryHistoryItem = new QSqlQuery(m_database);
        m_queryHistoryItem->prepare("INSERT OR IGNORE INTO History(URL, Title, VisitID) VALUES(:url, :title, :visitId)");

        m_queryVisit = new QSqlQuery(m_database);
        m_queryVisit->prepare("INSERT INTO Visits(VisitID, Date) VALUES(:visitId, :date)");
    }

    m_queryHistoryItem->bindValue(":url", item.URL.toString().toLower());
    m_queryHistoryItem->bindValue(":title", item.Title);
    m_queryHistoryItem->bindValue(":visitId", item.VisitID);
    if (!m_queryHistoryItem->exec())
        qDebug() << "[Error]: In HistoryManager::saveVisit - unable to save history item to database. Message: " << m_queryHistoryItem->lastError().text();

    m_queryVisit->bindValue(":visitId", item.VisitID);
    m_queryVisit->bindValue(":date", visitTime.toMSecsSinceEpoch());
    if (!m_queryVisit->exec())
        qDebug() << "[Error]: In HistoryManager::saveVisit - unable to save specific visit for URL " << item.URL.toString() << " at time " << visitTime.toString();

}
