/*
 * Copyright: 2013 Canonical, Ltd
 *
 * This file is part of reminders
 *
 * reminders is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * reminders is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors: Michael Zanetti <michael.zanetti@canonical.com>
 */

#include "notesstore.h"
#include "evernoteconnection.h"
#include "notebooks.h"
#include "notebook.h"
#include "note.h"
#include "tag.h"
#include "utils/enmldocument.h"
#include "utils/organizeradapter.h"
#include "userstore.h"
#include "logging.h"

#include "jobs/fetchnotesjob.h"
#include "jobs/fetchnotebooksjob.h"
#include "jobs/fetchnotejob.h"
#include "jobs/createnotejob.h"
#include "jobs/savenotejob.h"
#include "jobs/savenotebookjob.h"
#include "jobs/deletenotejob.h"
#include "jobs/createnotebookjob.h"
#include "jobs/expungenotebookjob.h"
#include "jobs/fetchtagsjob.h"
#include "jobs/createtagjob.h"
#include "jobs/savetagjob.h"
#include "jobs/expungetagjob.h"

#include "libintl.h"

#include <QImage>
#include <QStandardPaths>
#include <QUuid>
#include <QPointer>
#include <QDir>

NotesStore* NotesStore::s_instance = 0;

NotesStore::NotesStore(QObject *parent) :
    QAbstractListModel(parent),
    m_username("@invalid "),
    m_loading(false),
    m_notebooksLoading(false),
    m_tagsLoading(false)
{
    qCDebug(dcNotesStore) << "Creating NotesStore instance.";
    connect(UserStore::instance(), &UserStore::userChanged, this, &NotesStore::userStoreConnected);

    qRegisterMetaType<evernote::edam::NotesMetadataList>("evernote::edam::NotesMetadataList");
    qRegisterMetaType<evernote::edam::Note>("evernote::edam::Note");
    qRegisterMetaType<std::vector<evernote::edam::Notebook> >("std::vector<evernote::edam::Notebook>");
    qRegisterMetaType<evernote::edam::Notebook>("evernote::edam::Notebook");
    qRegisterMetaType<std::vector<evernote::edam::Tag> >("std::vector<evernote::edam::Tag>");
    qRegisterMetaType<evernote::edam::Tag>("evernote::edam::Tag");

    m_organizerAdapter = new OrganizerAdapter(this);

    QDir storageDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    qCDebug(dcNotesStore) << "Notes storare dir" << storageDir;
    if (!storageDir.exists()) {
        qCDebug(dcNotesStore) << "Creating storage directory:" << storageDir.absolutePath();
        storageDir.mkpath(storageDir.absolutePath());
    }
}

NotesStore *NotesStore::instance()
{
    if (!s_instance) {
        s_instance = new NotesStore();
    }
    return s_instance;
}

QString NotesStore::username() const
{
    return m_username;
}

void NotesStore::setUsername(const QString &username)
{
    if (username.isEmpty()) {
        // We don't accept an empty username.
        return;
    }
    if (!UserStore::instance()->userName().isEmpty() && username != UserStore::instance()->userName()) {
        qCWarning(dcNotesStore) << "Logged in to Evernote. Can't change account manually. User EvernoteConnection to log in to another account or log out and change this manually.";
        return;
    }

    if (m_username != username) {
        m_username = username;
        emit usernameChanged();

        QDir storageDir(storageLocation());
        foreach (const QString &fileName, storageDir.entryList({"*.lock"})) {
            qCDebug(dcNotesStore) << "Removing stale lock file" << storageLocation() + "/" + fileName;
            QFile f(storageLocation() +  "/" + fileName);
            f.remove();
        }

        m_cacheFile = storageLocation() + "notes.cache";
        qCDebug(dcNotesStore) << "Initialized cacheFile:" << m_cacheFile;
        loadFromCacheFile();
    }
}

QString NotesStore::storageLocation()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/" + m_username + "/";
}

void NotesStore::userStoreConnected()
{
    QString username = UserStore::instance()->userName();
    qCDebug(dcNotesStore) << "User store connected! Using username:" << username;
    setUsername(username);

    refreshNotebooks();
    refreshTags();
    refreshNotes();
}

bool NotesStore::loading() const
{
    return m_loading;
}

bool NotesStore::notebooksLoading() const
{
    return m_notebooksLoading;
}

bool NotesStore::tagsLoading() const
{
    return m_tagsLoading;
}

QString NotesStore::error() const
{
    return m_errorQueue.count() > 0 ? m_errorQueue.first() : QString();
}

int NotesStore::count() const
{
    return rowCount();
}

int NotesStore::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return m_notes.count();
}

QVariant NotesStore::data(const QModelIndex &index, int role) const
{
    switch (role) {
    case RoleGuid:
        return m_notes.at(index.row())->guid();
    case RoleNotebookGuid:
        return m_notes.at(index.row())->notebookGuid();
    case RoleCreated:
        return m_notes.at(index.row())->created();
    case RoleCreatedString:
        return m_notes.at(index.row())->createdString();
    case RoleUpdated:
        return m_notes.at(index.row())->updated();
    case RoleUpdatedString:
        return m_notes.at(index.row())->updatedString();
    case RoleTitle:
        return m_notes.at(index.row())->title();
    case RoleReminder:
        return m_notes.at(index.row())->reminder();
    case RoleReminderTime:
        return m_notes.at(index.row())->reminderTime();
    case RoleReminderTimeString:
        return m_notes.at(index.row())->reminderTimeString();
    case RoleReminderDone:
        return m_notes.at(index.row())->reminderDone();
    case RoleReminderDoneTime:
        return m_notes.at(index.row())->reminderDoneTime();
    case RoleEnmlContent:
        return m_notes.at(index.row())->enmlContent();
    case RoleHtmlContent:
        return m_notes.at(index.row())->htmlContent();
    case RoleRichTextContent:
        return m_notes.at(index.row())->richTextContent();
    case RolePlaintextContent:
        return m_notes.at(index.row())->plaintextContent();
    case RoleTagline:
        return m_notes.at(index.row())->tagline();
    case RoleResourceUrls:
        return m_notes.at(index.row())->resourceUrls();
    case RoleReminderSorting:
        // done reminders get +1000000000000 (this will break sorting in year 2286 :P)
        return QVariant::fromValue(m_notes.at(index.row())->reminderTime().toMSecsSinceEpoch() +
                (m_notes.at(index.row())->reminderDone() ? 10000000000000 : 0));
    case RoleTagGuids:
        return m_notes.at(index.row())->tagGuids();
    case RoleDeleted:
        return m_notes.at(index.row())->deleted();
    case RoleSynced:
        return m_notes.at(index.row())->synced();
    case RoleLoading:
        return m_notes.at(index.row())->loading();
    case RoleSyncError:
        return m_notes.at(index.row())->syncError();
    case RoleConflicting:
        return m_notes.at(index.row())->conflicting();
    }
    return QVariant();
}

QHash<int, QByteArray> NotesStore::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles.insert(RoleGuid, "guid");
    roles.insert(RoleNotebookGuid, "notebookGuid");
    roles.insert(RoleCreated, "created");
    roles.insert(RoleCreatedString, "createdString");
    roles.insert(RoleUpdated, "updated");
    roles.insert(RoleUpdatedString, "updatedString");
    roles.insert(RoleTitle, "title");
    roles.insert(RoleReminder, "reminder");
    roles.insert(RoleReminderTime, "reminderTime");
    roles.insert(RoleReminderTimeString, "reminderTimeString");
    roles.insert(RoleReminderDone, "reminderDone");
    roles.insert(RoleReminderDoneTime, "reminderDoneTime");
    roles.insert(RoleEnmlContent, "enmlContent");
    roles.insert(RoleRichTextContent, "richTextContent");
    roles.insert(RoleHtmlContent, "htmlContent");
    roles.insert(RolePlaintextContent, "plaintextContent");
    roles.insert(RoleTagline, "tagline");
    roles.insert(RoleResourceUrls, "resourceUrls");
    roles.insert(RoleTagGuids, "tagGuids");
    roles.insert(RoleDeleted, "deleted");
    roles.insert(RoleLoading, "loading");
    roles.insert(RoleSynced, "synced");
    roles.insert(RoleSyncError, "syncError");
    roles.insert(RoleConflicting, "conflicting");
    return roles;
}

NotesStore::~NotesStore()
{
}

QList<Note*> NotesStore::notes() const
{
    return m_notes;
}

Note *NotesStore::note(int index) const
{
    return m_notes.at(index);
}

Note *NotesStore::note(const QString &guid)
{
    return m_notesHash.value(guid);
}

QList<Notebook *> NotesStore::notebooks() const
{
    return m_notebooks;
}

Notebook *NotesStore::notebook(int index) const
{
    return m_notebooks.at(index);
}

Notebook *NotesStore::notebook(const QString &guid)
{
    return m_notebooksHash.value(guid);
}

void NotesStore::createNotebook(const QString &name)
{
    QString newGuid = QUuid::createUuid().toString();
    newGuid.remove("{").remove("}");
    qCDebug(dcNotesStore) << "Creating notebook:" << newGuid;
    Notebook *notebook = new Notebook(newGuid, 1, this);
    notebook->setName(name);
    if (m_notebooks.isEmpty()) {
        notebook->setIsDefaultNotebook(true);
    }

    m_notebooks.append(notebook);
    m_notebooksHash.insert(notebook->guid(), notebook);
    emit notebookAdded(notebook->guid());

    syncToCacheFile(notebook);

    if (EvernoteConnection::instance()->isConnected()) {
        qCDebug(dcSync) << "Creating notebook on server:" << notebook->guid();
        notebook->setLoading(true);
        CreateNotebookJob *job = new CreateNotebookJob(notebook);
        connect(job, &CreateNotebookJob::jobDone, this, &NotesStore::createNotebookJobDone);
        EvernoteConnection::instance()->enqueue(job);
    }
}

void NotesStore::createNotebookJobDone(EvernoteConnection::ErrorCode errorCode, const QString &errorMessage, const QString &tmpGuid, const evernote::edam::Notebook &result)
{
    Notebook *notebook = m_notebooksHash.value(tmpGuid);
    if (!notebook) {
        qCWarning(dcSync) << "Cannot find temporary notebook after create finished";
        return;
    }

    notebook->setLoading(false);

    handleUserError(errorCode);
    if (errorCode != EvernoteConnection::ErrorCodeNoError) {
        qCWarning(dcSync) << "Error creating notebook:" << errorMessage;
        notebook->setSyncError(true);
        emit notebookChanged(notebook->guid());
        return;
    }
    QString guid = QString::fromStdString(result.guid);

    qCDebug(dcSync)  << "Notebook created on server. Old guid:" << tmpGuid << "New guid:" << guid;
    qCDebug(dcNotesStore) << "Changing notebook guid. Old guid:" << tmpGuid << "New guid:" << guid;

    m_notebooksHash.insert(guid, notebook);
    notebook->setGuid(QString::fromStdString(result.guid));
    emit notebookGuidChanged(tmpGuid, notebook->guid());
    m_notebooksHash.remove(tmpGuid);

    notebook->setUpdateSequenceNumber(result.updateSequenceNum);
    notebook->setLastSyncedSequenceNumber(result.updateSequenceNum);
    notebook->setName(QString::fromStdString(result.name));
    emit notebookChanged(notebook->guid());

    QSettings cacheFile(m_cacheFile, QSettings::IniFormat);
    cacheFile.beginGroup("notebooks");
    cacheFile.remove(tmpGuid);
    cacheFile.endGroup();

    syncToCacheFile(notebook);

    foreach (const QString &noteGuid, notebook->m_notesList) {
        saveNote(noteGuid);
    }
}

void NotesStore::saveNotebook(const QString &guid)
{
    Notebook *notebook = m_notebooksHash.value(guid);
    if (!notebook) {
        qCWarning(dcNotesStore) << "Can't save notebook. Guid not found:" << guid;
        return;
    }

    notebook->setUpdateSequenceNumber(notebook->updateSequenceNumber()+1);
    syncToCacheFile(notebook);

    if (EvernoteConnection::instance()->isConnected()) {
        SaveNotebookJob *job = new SaveNotebookJob(notebook, this);
        connect(job, &SaveNotebookJob::jobDone, this, &NotesStore::saveNotebookJobDone);
        EvernoteConnection::instance()->enqueue(job);
        notebook->setLoading(true);
    }
    emit notebookChanged(notebook->guid());
}

void NotesStore::setDefaultNotebook(const QString &guid)
{
    Notebook *notebook = m_notebooksHash.value(guid);
    if (!notebook) {
        qCWarning(dcNotesStore) << "Notebook guid not found:" << guid;
        return;
    }

    qCDebug(dcNotesStore) << "Setting default notebook:" << guid;
    foreach (Notebook *tmp, m_notebooks) {
        if (tmp->isDefaultNotebook()) {
            tmp->setIsDefaultNotebook(false);
            saveNotebook(tmp->guid());
            break;
        }
    }
    notebook->setIsDefaultNotebook(true);
    saveNotebook(guid);
    emit defaultNotebookChanged(guid);
}

void NotesStore::saveTag(const QString &guid)
{
    Tag *tag = m_tagsHash.value(guid);
    if (!tag) {
        qCWarning(dcNotesStore) << "Can't save tag. Guid not found:" << guid;
        return;
    }

    tag->setUpdateSequenceNumber(tag->updateSequenceNumber()+1);
    syncToCacheFile(tag);

    if (EvernoteConnection::instance()->isConnected()) {
        tag->setLoading(true);
        emit tagChanged(tag->guid());
        SaveTagJob *job = new SaveTagJob(tag);
        connect(job, &SaveTagJob::jobDone, this, &NotesStore::saveTagJobDone);
        EvernoteConnection::instance()->enqueue(job);
    }
}

void NotesStore::expungeNotebook(const QString &guid)
{
#ifdef NO_EXPUNGE_NOTEBOOKS
    // This snipped can be used if the app is compiled with a restricted api key
    // that can't expunge notebooks on Evernote. Compile with
    // cmake -DNO_EXPUNGE_NOTEBOOKS=1
    if (m_username != "@local") {
        qCWarning(dcNotesStore) << "Account managed by Evernote. Cannot delete notebooks.";
        m_errorQueue.append(QString(gettext("This account is managed by Evernote. Use the Evernote website to delete notebooks.")));
        emit errorChanged();
        return;
    }
#endif

    Notebook* notebook = m_notebooksHash.value(guid);
    if (!notebook) {
        qCWarning(dcNotesStore) << "Cannot delete notebook. Notebook not found for guid:" << guid;
        return;
    }

    if (notebook->isDefaultNotebook()) {
        qCWarning(dcNotesStore) << "Cannot delete the default notebook.";
        m_errorQueue.append(QString(gettext("Cannot delete the default notebook. Set another notebook to be the default first.")));
        emit errorChanged();
        return;
    }

    if (notebook->noteCount() > 0) {
        QString defaultNotebook;
        foreach (const Notebook *notebook, m_notebooks) {
            if (notebook->isDefaultNotebook()) {
                defaultNotebook = notebook->guid();
                break;
            }
        }
        if (defaultNotebook.isEmpty()) {
            qCWarning(dcNotesStore) << "No default notebook set. Can't delete notebooks.";
            return;
        }

        while (notebook->noteCount() > 0) {
            QString noteGuid = notebook->noteAt(0);
            Note *note = m_notesHash.value(noteGuid);
            if (!note) {
                qCWarning(dcNotesStore) << "Notebook holds a noteGuid which cannot be found in notes store";
                Q_ASSERT(false);
                continue;
            }
            qCDebug(dcNotesStore) << "Moving note" << noteGuid << "to default Notebook";
            note->setNotebookGuid(defaultNotebook);
            saveNote(note->guid());
            emit noteChanged(note->guid(), defaultNotebook);
            syncToCacheFile(note);
        }
    }

    if (notebook->lastSyncedSequenceNumber() == 0) {
        emit notebookRemoved(notebook->guid());
        m_notebooks.removeAll(notebook);
        m_notebooksHash.remove(notebook->guid());
        emit notebookRemoved(notebook->guid());

        QSettings settings(m_cacheFile, QSettings::IniFormat);
        settings.beginGroup("notebooks");
        settings.remove(notebook->guid());
        settings.endGroup();

        notebook->deleteInfoFile();
        notebook->deleteLater();
    } else {
        qCDebug(dcNotesStore) << "Setting notebook to deleted:" << notebook->guid();
        notebook->setDeleted(true);
        notebook->setUpdateSequenceNumber(notebook->updateSequenceNumber()+1);
        emit notebookChanged(notebook->guid());
        syncToCacheFile(notebook);

        if (EvernoteConnection::instance()->isConnected()) {
            ExpungeNotebookJob *job = new ExpungeNotebookJob(guid, this);
            connect(job, &ExpungeNotebookJob::jobDone, this, &NotesStore::expungeNotebookJobDone);
            EvernoteConnection::instance()->enqueue(job);
        }
    }
}

QList<Tag *> NotesStore::tags() const
{
    return m_tags;
}

Tag *NotesStore::tag(const QString &guid)
{
    return m_tagsHash.value(guid);
}

Tag* NotesStore::createTag(const QString &name)
{
    foreach (Tag *tag, m_tags) {
        if (tag->name() == name) {
            return tag;
        }
    }

    QString newGuid = QUuid::createUuid().toString();
    newGuid.remove("{").remove("}");
    Tag *tag = new Tag(newGuid, 1, this);
    tag->setName(name);
    m_tags.append(tag);
    m_tagsHash.insert(tag->guid(), tag);
    emit tagAdded(tag->guid());

    syncToCacheFile(tag);

    if (EvernoteConnection::instance()->isConnected()) {
        CreateTagJob *job = new CreateTagJob(tag);
        connect(job, &CreateTagJob::jobDone, this, &NotesStore::createTagJobDone);
        EvernoteConnection::instance()->enqueue(job);
    }
    return tag;
}

void NotesStore::createTagJobDone(EvernoteConnection::ErrorCode errorCode, const QString &errorMessage, const QString &tmpGuid, const evernote::edam::Tag &result)
{
    Tag *tag = m_tagsHash.value(tmpGuid);
    if (!tag) {
        qCWarning(dcSync) << "Create Tag job done but tag can't be found any more";
        return;
    }

    tag->setLoading(false);

    handleUserError(errorCode);
    if (errorCode != EvernoteConnection::ErrorCodeNoError) {
        qCWarning(dcSync) << "Error creating tag on server:" << errorMessage;
        tag->setSyncError(true);
        emit tagChanged(tag->guid());
        return;
    }

    QString guid = QString::fromStdString(result.guid);
    m_tagsHash.insert(guid, tag);
    tag->setGuid(QString::fromStdString(result.guid));
    emit tagGuidChanged(tmpGuid, guid);
    m_tagsHash.remove(tmpGuid);

    tag->setUpdateSequenceNumber(result.updateSequenceNum);
    tag->setLastSyncedSequenceNumber(result.updateSequenceNum);
    emit tagChanged(tag->guid());

    QSettings cacheFile(m_cacheFile, QSettings::IniFormat);
    cacheFile.beginGroup("tags");
    cacheFile.remove(tmpGuid);
    cacheFile.endGroup();

    syncToCacheFile(tag);

    foreach (const QString &noteGuid, tag->m_notesList) {
        saveNote(noteGuid);
    }
}

void NotesStore::saveTagJobDone(EvernoteConnection::ErrorCode errorCode, const QString &errorMessage, const evernote::edam::Tag &result)
{
    Tag *tag = m_tagsHash.value(QString::fromStdString(result.guid));
    if (!tag) {
        qCWarning(dcSync) << "Save tag job finished, but tag can't be found any more";
        return;
    }
    tag->setLoading(false);

    handleUserError(errorCode);
    if (errorCode != EvernoteConnection::ErrorCodeNoError) {
        qCWarning(dcSync) << "Error updating tag on server" << errorMessage;
        tag->setSyncError(true);
        emit tagChanged(tag->guid());
        return;
    }

    tag->setName(QString::fromStdString(result.name));
    tag->setUpdateSequenceNumber(result.updateSequenceNum);
    tag->setLastSyncedSequenceNumber(result.updateSequenceNum);
    emit tagChanged(tag->guid());
    syncToCacheFile(tag);
}

void NotesStore::expungeTagJobDone(EvernoteConnection::ErrorCode errorCode, const QString &errorMessage, const QString &guid)
{
    handleUserError(errorCode);
    if (errorCode != EvernoteConnection::ErrorCodeNoError) {
        qCWarning(dcSync) << "Error expunging tag:" << errorMessage;
        return;
    }

    if (!m_tagsHash.contains(guid)) {
        qCWarning(dcSync) << "Received a response for a expungeTag call, but can't find tag around any more.";
        return;
    }

    emit tagRemoved(guid);
    Tag *tag = m_tagsHash.take(guid);
    m_tags.removeAll(tag);

    QSettings cacheFile(m_cacheFile, QSettings::IniFormat);
    cacheFile.beginGroup("tags");
    cacheFile.remove(guid);
    cacheFile.endGroup();
    tag->syncToInfoFile();

    tag->deleteInfoFile();
    tag->deleteLater();
}

void NotesStore::tagNote(const QString &noteGuid, const QString &tagGuid)
{
    Note *note = m_notesHash.value(noteGuid);
    if (!note) {
        qCWarning(dcNotesStore) << "No such note" << noteGuid;
        return;
    }

    Tag *tag = m_tagsHash.value(tagGuid);
    if (!tag) {
        qCWarning(dcNotesStore) << "No such tag" << tagGuid;
        return;
    }

    if (note->tagGuids().contains(tagGuid)) {
        qCWarning(dcNotesStore) << "Note" << noteGuid << "already tagged with tag" << tagGuid;
        return;
    }

    note->setTagGuids(note->tagGuids() << tagGuid);
    saveNote(noteGuid);
}

void NotesStore::untagNote(const QString &noteGuid, const QString &tagGuid)
{
    Note *note = m_notesHash.value(noteGuid);
    if (!note) {
        qCWarning(dcNotesStore) << "No such note" << noteGuid;
        return;
    }

    Tag *tag = m_tagsHash.value(tagGuid);
    if (!tag) {
        qCWarning(dcNotesStore) << "No such tag" << tagGuid;
        return;
    }

    if (!note->tagGuids().contains(tagGuid)) {
        qCWarning(dcNotesStore) << "Note" << noteGuid << "is not tagged with tag" << tagGuid;
        return;
    }

    QStringList newTagGuids = note->tagGuids();
    newTagGuids.removeAll(tagGuid);
    note->setTagGuids(newTagGuids);
    saveNote(noteGuid);
}

void NotesStore::refreshNotes(const QString &filterNotebookGuid, int startIndex)
{
    if (m_loading && startIndex == 0) {
        qCWarning(dcSync) << "Still busy with refreshing...";
        return;
    }

    if (EvernoteConnection::instance()->isConnected()) {
        m_loading = true;
        emit loadingChanged();

        if (startIndex == 0) {
            m_unhandledNotes = m_notesHash.keys();
        }

        FetchNotesJob *job = new FetchNotesJob(filterNotebookGuid, QString(), startIndex);
        connect(job, &FetchNotesJob::jobDone, this, &NotesStore::fetchNotesJobDone);
        EvernoteConnection::instance()->enqueue(job);
    }
}

void NotesStore::fetchNotesJobDone(EvernoteConnection::ErrorCode errorCode, const QString &errorMessage, const evernote::edam::NotesMetadataList &results, const QString &filterNotebookGuid)
{
    handleUserError(errorCode);
    if (errorCode != EvernoteConnection::ErrorCodeNoError) {
        qCWarning(dcSync) << "FetchNotesJobDone: Failed to fetch notes list:" << errorMessage << errorCode;
        m_loading = false;
        emit loadingChanged();
        return;
    }

    for (unsigned int i = 0; i < results.notes.size(); ++i) {
        evernote::edam::NoteMetadata result = results.notes.at(i);
        Note *note = m_notesHash.value(QString::fromStdString(result.guid));
        m_unhandledNotes.removeAll(QString::fromStdString(result.guid));
        QVector<int> changedRoles;
        bool newNote = note == 0;
        if (newNote) {
            qCDebug(dcSync) << "Found new note on server. Creating local copy:" << QString::fromStdString(result.guid);
            note = new Note(QString::fromStdString(result.guid), 0, this);
            connect(note, &Note::reminderChanged, this, &NotesStore::emitDataChanged);
            connect(note, &Note::reminderDoneChanged, this, &NotesStore::emitDataChanged);

            updateFromEDAM(result, note);
            beginInsertRows(QModelIndex(), m_notes.count(), m_notes.count());
            m_notesHash.insert(note->guid(), note);
            m_notes.append(note);
            endInsertRows();
            emit noteAdded(note->guid(), note->notebookGuid());
            emit countChanged();
            syncToCacheFile(note);

        } else if (note->synced()) {
            // Local note did not change. Check if we need to refresh from server.
            if (note->updateSequenceNumber() < result.updateSequenceNum) {
                qCDebug(dcSync) << "refreshing note from network. suequence number changed: " << note->updateSequenceNumber() << "->" << result.updateSequenceNum;
                changedRoles = updateFromEDAM(result, note);
                refreshNoteContent(note->guid(), FetchNoteJob::LoadContent, EvernoteJob::JobPriorityMedium);
                syncToCacheFile(note);
            }
        } else {
            // Local note changed. See if we can push our changes.
            if (note->lastSyncedSequenceNumber() == result.updateSequenceNum) {
                qCDebug(dcSync) << "Local note" << note->guid() << "has changed while server note did not. Pushing changes.";

                // Make sure we have everything loaded from cache before saving to server
                if (!note->loaded() && note->isCached()) {
                    note->loadFromCacheFile();
                }

                note->setLoading(true);
                changedRoles << RoleLoading;
                SaveNoteJob *job = new SaveNoteJob(note, this);
                connect(job, &SaveNoteJob::jobDone, this, &NotesStore::saveNoteJobDone);
                EvernoteConnection::instance()->enqueue(job);
            } else {
                qCWarning(dcSync) << "********************************************************";
                qCWarning(dcSync) << "* CONFLICT: Note has been changed on server and locally!";
                qCWarning(dcSync) << "* local note sequence:" << note->updateSequenceNumber();
                qCWarning(dcSync) << "* last synced sequence:" << note->lastSyncedSequenceNumber();
                qCWarning(dcSync) << "* remote update sequence:" << result.updateSequenceNum;
                qCWarning(dcSync) << "********************************************************";
                note->setConflicting(true);
                changedRoles << RoleConflicting;

                // Not setting parent as we don't want to squash the reply.
                FetchNoteJob::LoadWhatFlags flags = 0x0;
                flags |= FetchNoteJob::LoadContent;
                flags |= FetchNoteJob::LoadResources;
                FetchNoteJob *fetchNoteJob = new FetchNoteJob(note->guid(), flags);
                fetchNoteJob->setJobPriority(EvernoteJob::JobPriorityMedium);
                connect(fetchNoteJob, &FetchNoteJob::resultReady, this, &NotesStore::fetchConflictingNoteJobDone);
                EvernoteConnection::instance()->enqueue(fetchNoteJob);
            }
        }

        if (!results.searchedWords.empty()) {
            note->setIsSearchResult(true);
            changedRoles << RoleIsSearchResult;
        }

        if (changedRoles.count() > 0) {
            QModelIndex noteIndex = index(m_notes.indexOf(note));
            emit dataChanged(noteIndex, noteIndex, changedRoles);
            emit noteChanged(note->guid(), note->notebookGuid());
        }
    }

    if (results.startIndex + (int32_t)results.notes.size() < results.totalNotes) {
        qCDebug(dcSync) << "Not all notes fetched yet. Fetching next batch.";
        refreshNotes(filterNotebookGuid, results.startIndex + results.notes.size());
    } else {
        qCDebug(dcSync) << "Fetched all notes from Evernote. Starting sync of local-only notes.";
        m_organizerAdapter->startSync();
        m_loading = false;
        emit loadingChanged();


        foreach (const QString &unhandledGuid, m_unhandledNotes) {
            Note *note = m_notesHash.value(unhandledGuid);
            if (!note) {
                continue; // Note might be deleted locally by now
            }
            qCDebug(dcSync) << "Have a local note that's not available on server!" << note->guid();
            if (note->lastSyncedSequenceNumber() == 0) {
                // This note hasn't been created on the server yet. Do that now.
                bool hasUnsyncedTag = false;
                foreach (const QString &tagGuid, note->tagGuids()) {
                    Tag *tag = m_tagsHash.value(tagGuid);
                    Q_ASSERT_X(tag, "FetchNotesJob done", "note->tagGuids() contains a non existing tag.");
                    if (tag && tag->lastSyncedSequenceNumber() == 0) {
                        hasUnsyncedTag = true;
                        break;
                    }
                }
                if (hasUnsyncedTag) {
                    qCDebug(dcSync) << "Not syncing note to server yet. Have a tag that needs sync first";
                    continue;
                }
                Notebook *notebook = m_notebooksHash.value(note->notebookGuid());
                if (notebook && notebook->lastSyncedSequenceNumber() == 0) {
                    qCDebug(dcSync) << "Not syncing note to server yet. The notebook needs to be synced first";
                    continue;
                }
                qCDebug(dcSync) << "Creating note on server:" << note->guid();

                // Make sure we have everything loaded from cache before saving to server
                if (!note->loaded() && note->isCached()) {
                    note->loadFromCacheFile();
                }

                QModelIndex idx = index(m_notes.indexOf(note));
                note->setLoading(true);
                emit dataChanged(idx, idx, QVector<int>() << RoleLoading);
                CreateNoteJob *job = new CreateNoteJob(note, this);
                connect(job, &CreateNoteJob::jobDone, this, &NotesStore::createNoteJobDone);
                EvernoteConnection::instance()->enqueue(job);
            } else {
                int idx = m_notes.indexOf(note);
                if (idx == -1) {
                    qCWarning(dcSync) << "Should sync unhandled note but it is gone by now...";
                    continue;
                }

                if (note->synced()) {
                    qCDebug(dcSync) << "Note has been deleted from the server and not changed locally. Deleting local note:" << note->guid();
                    removeNote(note->guid());
                } else {
                    qCDebug(dcSync) << "CONFLICT: Note has been deleted from the server but we have unsynced local changes for note:" << note->guid();
                    FetchNoteJob::LoadWhatFlags flags = 0x0;
                    flags |= FetchNoteJob::LoadContent;
                    flags |= FetchNoteJob::LoadResources;
                    FetchNoteJob *job = new FetchNoteJob(note->guid(), flags);
                    connect(job, &FetchNoteJob::resultReady, this, &NotesStore::fetchConflictingNoteJobDone);
                    EvernoteConnection::instance()->enqueue(job);

                    note->setConflicting(true);
                    emit dataChanged(index(idx), index(idx), QVector<int>() << RoleConflicting);
                }
            }
        }
        qCDebug(dcSync) << "Local-only notes synced.";
    }
}

void NotesStore::refreshNoteContent(const QString &guid, FetchNoteJob::LoadWhat what, EvernoteJob::JobPriority priority)
{
    Note *note = m_notesHash.value(guid);
    if (!note) {
        qCWarning(dcSync) << "RefreshNoteContent: Can't refresn note content. Note guid not found:" << guid;
        return;
    }
    if (EvernoteConnection::instance()->isConnected()) {
        qCDebug(dcNotesStore) << "Fetching note content from network for note" << guid << (what == FetchNoteJob::LoadContent ? "Content" : "Resource") << "Priority:" << priority;
        FetchNoteJob *job = new FetchNoteJob(guid, what, this);
        job->setJobPriority(priority);
        connect(job, &FetchNoteJob::resultReady, this, &NotesStore::fetchNoteJobDone);
        EvernoteConnection::instance()->enqueue(job);

        if (!note->loading()) {
            note->setLoading(true);
            int idx = m_notes.indexOf(note);
            emit dataChanged(index(idx), index(idx), QVector<int>() << RoleLoading);
        }
    }
}

void NotesStore::fetchNoteJobDone(EvernoteConnection::ErrorCode errorCode, const QString &errorMessage, const evernote::edam::Note &result, FetchNoteJob::LoadWhatFlags what)
{
    FetchNoteJob *job = static_cast<FetchNoteJob*>(sender());
    Note *note = m_notesHash.value(QString::fromStdString(result.guid));
    if (!note) {
        qCWarning(dcSync) << "can't find note for this update... ignoring...";
        return;
    }
    if (note->updateSequenceNumber() > result.updateSequenceNum) {
        qCWarning(dcSync) << "Local update sequence number higher than remote. Local:" << note->updateSequenceNumber() << "remote:" << result.updateSequenceNum;
        return;
    }

    QModelIndex noteIndex = index(m_notes.indexOf(note));
    QVector<int> roles;

    handleUserError(errorCode);
    if (errorCode != EvernoteConnection::ErrorCodeNoError) {
        qCWarning(dcSync) << "Fetch note job failed:" << errorMessage;
        note->setLoading(false);
        roles << RoleLoading;
        note->setSyncError(true);
        roles << RoleSyncError;
        emit dataChanged(noteIndex, noteIndex, roles);
        return;
    }

    if (result.deleted > 0) {
        qCDebug(dcSync) << "Note has been deleted on server. Deleting locally.";
        removeNote(note->guid());
        return;
    }

    if (note->notebookGuid() != QString::fromStdString(result.notebookGuid)) {
        note->setNotebookGuid(QString::fromStdString(result.notebookGuid));
        roles << RoleGuid;
    }
    if (note->title() != QString::fromStdString(result.title)) {
        note->setTitle(QString::fromStdString(result.title));
        roles << RoleTitle;
    }
    if (note->updated() != QDateTime::fromMSecsSinceEpoch(result.updated)) {
        note->setUpdated(QDateTime::fromMSecsSinceEpoch(result.updated));
        roles << RoleUpdated << RoleUpdatedString;
    }
    QStringList tagGuids;
    for (quint32 i = 0; i < result.tagGuids.size(); i++) {
        QString tag = QString::fromStdString(result.tagGuids.at(i));
        if (m_tagsHash.contains(tag)) {
            refreshTags();
        }
        tagGuids << tag;
    }
    if (note->tagGuids() != tagGuids) {
        note->setTagGuids(tagGuids);
        roles << RoleTagGuids;
    }

    // Notes are fetched without resources by default. if we discover one or more resources where we don't have
    // data in the cache, let's refresh the note again with resource data.
    bool refreshWithResourceData = false;

    qCDebug(dcSync) << "got note content" << note->guid() << (what == FetchNoteJob::LoadContent ? "content" : "image") << result.resources.size();
    // Resources need to be set before the content because otherwise the image provider won't find them when the content is updated in the ui
    for (unsigned int i = 0; i < result.resources.size(); ++i) {

        evernote::edam::Resource resource = result.resources.at(i);

        QString hash = QByteArray::fromRawData(resource.data.bodyHash.c_str(), resource.data.bodyHash.length()).toHex();
        QString fileName = QString::fromStdString(resource.attributes.fileName);
        QString mime = QString::fromStdString(resource.mime);

        if (what == FetchNoteJob::LoadResources) {
            qCDebug(dcSync) << "Resource content fetched for note:" << note->guid() << "Filename:" << fileName << "Mimetype:" << mime << "Hash:" << hash;
            QByteArray resourceData = QByteArray(resource.data.body.data(), resource.data.size);
            note->addResource(hash, fileName, mime, resourceData);
        } else {
            qCDebug(dcSync) << "Adding resource info to note:" << note->guid() << "Filename:" << fileName << "Mimetype:" << mime << "Hash:" << hash;
            Resource *resource = note->addResource(hash, fileName, mime);

            if (!resource->isCached()) {
                qCDebug(dcSync) << "Resource not yet fetched for note:" << note->guid() << "Filename:" << fileName << "Mimetype:" << mime << "Hash:" << hash;
                refreshWithResourceData = true;
            }
        }
        roles << RoleHtmlContent << RoleEnmlContent << RoleResourceUrls;
    }

    if (what == FetchNoteJob::LoadContent) {
        note->setEnmlContent(QString::fromStdString(result.content));
        note->setUpdateSequenceNumber(result.updateSequenceNum);
        note->setLastSyncedSequenceNumber(result.updateSequenceNum);
        roles << RoleHtmlContent << RoleEnmlContent << RoleTagline << RolePlaintextContent;
    }
    bool syncReminders = false;
    if (note->reminderOrder() != result.attributes.reminderOrder) {
        note->setReminderOrder(result.attributes.reminderOrder);
        roles << RoleReminder;
        syncReminders = true;
    }
    QDateTime reminderTime;
    if (result.attributes.reminderTime > 0) {
        reminderTime = QDateTime::fromMSecsSinceEpoch(result.attributes.reminderTime);
    }
    if (note->reminderTime() != reminderTime) {
        note->setReminderTime(reminderTime);
        roles << RoleReminderTime << RoleReminderTimeString;
        syncReminders = true;
    }
    QDateTime reminderDoneTime;
    if (result.attributes.reminderDoneTime > 0) {
        reminderDoneTime = QDateTime::fromMSecsSinceEpoch(result.attributes.reminderDoneTime);
    }
    if (note->reminderDoneTime() != reminderDoneTime) {
        note->setReminderDoneTime(reminderDoneTime);
        roles << RoleReminderDone << RoleReminderDoneTime;
        syncReminders = true;
    }
    if (syncReminders) {
        m_organizerAdapter->startSync();
    }

    note->setLoading(false);
    roles << RoleLoading;

    emit noteChanged(note->guid(), note->notebookGuid());
    emit dataChanged(noteIndex, noteIndex, roles);

    if (refreshWithResourceData) {
        qCDebug(dcSync) << "Fetching Note resources:" << note->guid();
        EvernoteJob::JobPriority newPriority = job->jobPriority() == EvernoteJob::JobPriorityMedium ? EvernoteJob::JobPriorityLow : job->jobPriority();
        refreshNoteContent(note->guid(), FetchNoteJob::LoadResources, newPriority);
    }
    syncToCacheFile(note); // Syncs into the list cache
    note->syncToCacheFile(); // Syncs note's content into notes cache
}

void NotesStore::fetchConflictingNoteJobDone(EvernoteConnection::ErrorCode errorCode, const QString &errorMessage, const evernote::edam::Note &result, FetchNoteJob::LoadWhatFlags what)
{
    Q_UNUSED(what) // We always fetch everything when sensing a conflict

    Note *note = m_notesHash.value(QString::fromStdString(result.guid));
    if (!note) {
        qCWarning(dcSync) << "Fetched conflicting note from server but local note can't be found any more:" << QString::fromStdString(result.guid);
        return;
    }

    handleUserError(errorCode);
    if (errorCode != EvernoteConnection::ErrorCodeNoError) {
        qCWarning(dcSync) << "Failed to fetch conflicting note for" << note->guid() << errorMessage;
        return;
    }

    qCDebug(dcSync) << "Fetched conflicting note:" << note->guid();

    // Make sure local note is loaded
    note->loadFromCacheFile();

    Note *serverNote = new Note("conflict-" + note->guid(), result.updateSequenceNum, note);
    serverNote->setUpdateSequenceNumber(result.updateSequenceNum);
    serverNote->setLastSyncedSequenceNumber(result.updateSequenceNum);
    serverNote->setTitle(QString::fromStdString(result.title));
    serverNote->setNotebookGuid(QString::fromStdString(result.notebookGuid));
    serverNote->setCreated(QDateTime::fromMSecsSinceEpoch(result.created));
    serverNote->setUpdated(QDateTime::fromMSecsSinceEpoch(result.updated));
    serverNote->setDeleted(result.deleted > 0);
    QStringList tagGuids;
    foreach (const std::string &guid, result.tagGuids) {
        tagGuids << QString::fromStdString(guid);
    }
    serverNote->setTagGuids(tagGuids);
    serverNote->setReminderOrder(result.attributes.reminderOrder);
    serverNote->setReminderTime(QDateTime::fromMSecsSinceEpoch(result.attributes.reminderTime));
    serverNote->setReminderDoneTime(QDateTime::fromMSecsSinceEpoch(result.attributes.reminderDoneTime));

    serverNote->setEnmlContent(QString::fromStdString(result.content));

    foreach (const evernote::edam::Resource &resource, result.resources) {
        serverNote->addResource(QString::fromStdString(resource.data.bodyHash), QString::fromStdString(resource.attributes.fileName), QString::fromStdString(resource.mime));
    }

    note->setConflictingNote(serverNote);
    note->setLoading(false);

}

void NotesStore::refreshNotebooks()
{
    if (!EvernoteConnection::instance()->isConnected()) {
        qCWarning(dcSync) << "Not connected. Cannot fetch notebooks from server.";
        return;
    }

    m_notebooksLoading = true;
    emit notebooksLoadingChanged();
    FetchNotebooksJob *job = new FetchNotebooksJob();
    connect(job, &FetchNotebooksJob::jobDone, this, &NotesStore::fetchNotebooksJobDone);
    EvernoteConnection::instance()->enqueue(job);
}

void NotesStore::fetchNotebooksJobDone(EvernoteConnection::ErrorCode errorCode, const QString &errorMessage, const std::vector<evernote::edam::Notebook> &results)
{
    m_notebooksLoading = false;
    emit notebooksLoadingChanged();

    handleUserError(errorCode);
    if (errorCode != EvernoteConnection::ErrorCodeNoError) {
        qCWarning(dcSync) << "FetchNotebooksJobDone: Failed to fetch notes list:" << errorMessage << errorCode;
        return;
    }

    QList<Notebook*> unhandledNotebooks = m_notebooks;

    qCDebug(dcSync) << "Received" << results.size() << "notebooks from Evernote.";
    for (unsigned int i = 0; i < results.size(); ++i) {
        evernote::edam::Notebook result = results.at(i);
        Notebook *notebook = m_notebooksHash.value(QString::fromStdString(result.guid));
        unhandledNotebooks.removeAll(notebook);
        bool newNotebook = notebook == 0;
        if (newNotebook) {
            qCDebug(dcSync) << "Found new notebook on Evernote:" << QString::fromStdString(result.guid);
            notebook = new Notebook(QString::fromStdString(result.guid), 0, this);
            updateFromEDAM(result, notebook);
            m_notebooksHash.insert(notebook->guid(), notebook);
            m_notebooks.append(notebook);
            emit notebookAdded(notebook->guid());
            syncToCacheFile(notebook);
        } else if (notebook->synced()) {
            if (notebook->updateSequenceNumber() < result.updateSequenceNum) {
                qCDebug(dcSync) << "Notebook on Evernote is newer than local copy. Updating:" << notebook->guid();
                updateFromEDAM(result, notebook);
                emit notebookChanged(notebook->guid());
                syncToCacheFile(notebook);
            }
        } else {
            if (result.updateSequenceNum == notebook->lastSyncedSequenceNumber()) {
                // Local notebook changed. See if we can push our changes
                if (notebook->deleted()) {
                    qCDebug(dcNotesStore) << "Local notebook has been deleted. Deleting from server";
                    expungeNotebook(notebook->guid());
                } else {
                    qCDebug(dcNotesStore) << "Local Notebook changed. Uploading changes to Evernote:" << notebook->guid();
                    SaveNotebookJob *job = new SaveNotebookJob(notebook);
                    connect(job, &SaveNotebookJob::jobDone, this, &NotesStore::saveNotebookJobDone);
                    EvernoteConnection::instance()->enqueue(job);
                    notebook->setLoading(true);
                    emit notebookChanged(notebook->guid());
                }
            } else {
                qCWarning(dcNotesStore) << "Sync conflict in notebook:" << notebook->name();
                qCWarning(dcNotesStore) << "Resolving of sync conflicts is not implemented yet.";
                notebook->setSyncError(true);
                emit notebookChanged(notebook->guid());
            }
        }
    }

    qCDebug(dcSync) << "Remote notebooks merged into storage. Merging local changes to server.";

    foreach (Notebook *notebook, unhandledNotebooks) {
        if (notebook->lastSyncedSequenceNumber() == 0) {
            qCDebug(dcSync) << "Have a local notebook that doesn't exist on Evernote. Creating on server:" << notebook->guid();
            notebook->setLoading(true);
            CreateNotebookJob *job = new CreateNotebookJob(notebook);
            connect(job, &CreateNotebookJob::jobDone, this, &NotesStore::createNotebookJobDone);
            EvernoteConnection::instance()->enqueue(job);
            emit notebookChanged(notebook->guid());
        } else {
            qCDebug(dcSync) << "Notebook has been deleted on the server. Deleting local copy:" << notebook->guid();
            m_notebooks.removeAll(notebook);
            m_notebooksHash.remove(notebook->guid());
            emit notebookRemoved(notebook->guid());

            QSettings settings(m_cacheFile, QSettings::IniFormat);
            settings.beginGroup("notebooks");
            settings.remove(notebook->guid());
            settings.endGroup();

            notebook->deleteInfoFile();
            notebook->deleteLater();
        }
    }

    qCDebug(dcSync) << "Notebooks merged.";
}

void NotesStore::refreshTags()
{
    if (!EvernoteConnection::instance()->isConnected()) {
        qCWarning(dcSync) << "Not connected. Cannot fetch tags from server.";
        return;
    }
    m_tagsLoading = true;
    emit tagsLoadingChanged();
    FetchTagsJob *job = new FetchTagsJob();
    connect(job, &FetchTagsJob::jobDone, this, &NotesStore::fetchTagsJobDone);
    EvernoteConnection::instance()->enqueue(job);
}

void NotesStore::clearError()
{
    if (m_errorQueue.count() > 0) {
        m_errorQueue.takeFirst();
        emit errorChanged();
    }
}

void NotesStore::fetchTagsJobDone(EvernoteConnection::ErrorCode errorCode, const QString &errorMessage, const std::vector<evernote::edam::Tag> &results)
{
    m_tagsLoading = false;
    emit tagsLoadingChanged();

    handleUserError(errorCode);
    if (errorCode != EvernoteConnection::ErrorCodeNoError) {
        qCWarning(dcSync) << "FetchTagsJobDone: Failed to fetch notes list:" << errorMessage << errorCode;
        return;
    }

    QHash<QString, Tag*> unhandledTags = m_tagsHash;
    for (unsigned int i = 0; i < results.size(); ++i) {
        evernote::edam::Tag result = results.at(i);
        unhandledTags.remove(QString::fromStdString(result.guid));
        Tag *tag = m_tagsHash.value(QString::fromStdString(result.guid));
        bool newTag = tag == 0;
        if (newTag) {
            tag = new Tag(QString::fromStdString(result.guid), result.updateSequenceNum, this);
            tag->setLastSyncedSequenceNumber(result.updateSequenceNum);
            qCDebug(dcSync) << "got new tag with seq:" << result.updateSequenceNum << tag->synced() << tag->updateSequenceNumber() << tag->lastSyncedSequenceNumber();
            tag->setName(QString::fromStdString(result.name));
            m_tagsHash.insert(tag->guid(), tag);
            m_tags.append(tag);
            emit tagAdded(tag->guid());
            syncToCacheFile(tag);
        } else if (tag->synced()) {
            if (tag->updateSequenceNumber() < result.updateSequenceNum) {
                tag->setName(QString::fromStdString(result.name));
                tag->setUpdateSequenceNumber(result.updateSequenceNum);
                tag->setLastSyncedSequenceNumber(result.updateSequenceNum);
                emit tagChanged(tag->guid());
                syncToCacheFile(tag);
            }
        } else {
            // local tag changed. See if we can sync it to the server
            if (result.updateSequenceNum == tag->lastSyncedSequenceNumber()) {
                if (tag->deleted()) {
                    qCDebug(dcNotesStore) << "Tag has been deleted locally";
                    expungeTag(tag->guid());
                } else {
                    SaveTagJob *job = new SaveTagJob(tag);
                    connect(job, &SaveTagJob::jobDone, this, &NotesStore::saveTagJobDone);
                    EvernoteConnection::instance()->enqueue(job);
                    tag->setLoading(true);
                    emit tagChanged(tag->guid());
                }
            } else {
                qCWarning(dcSync) << "CONFLICT in tag" << tag->name();
                tag->setSyncError(true);
                emit tagChanged(tag->guid());
            }
        }


    }

    foreach (Tag *tag, unhandledTags) {
        if (tag->lastSyncedSequenceNumber() == 0) {
            tag->setLoading(true);
            CreateTagJob *job = new CreateTagJob(tag);
            connect(job, &CreateTagJob::jobDone, this, &NotesStore::createTagJobDone);
            EvernoteConnection::instance()->enqueue(job);
            emit tagChanged(tag->guid());
        } else {
            m_tags.removeAll(tag);
            m_tagsHash.remove(tag->guid());
            emit tagRemoved(tag->guid());

            tag->deleteInfoFile();
            tag->deleteLater();
        }
    }
}

Note* NotesStore::createNote(const QString &title, const QString &notebookGuid, const QString &richTextContent)
{
    EnmlDocument enmlDoc;
    enmlDoc.setRichText(richTextContent);
    return createNote(title, notebookGuid, enmlDoc);
}

Note* NotesStore::createNote(const QString &title, const QString &notebookGuid, const EnmlDocument &content)
{
    QString newGuid = QUuid::createUuid().toString();
    newGuid.remove("{").remove("}");
    Note *note = new Note(newGuid, 1, this);
    connect(note, &Note::reminderChanged, this, &NotesStore::emitDataChanged);
    connect(note, &Note::reminderDoneChanged, this, &NotesStore::emitDataChanged);

    note->setTitle(title);

    if (!notebookGuid.isEmpty()) {
        note->setNotebookGuid(notebookGuid);
    } else if (m_notebooks.count() > 0){
        QString generatedNotebook = m_notebooks.first()->guid();
        foreach (Notebook *notebook, m_notebooks) {
            if (notebook->isDefaultNotebook()) {
                generatedNotebook = notebook->guid();
                break;
            }
        }
        note->setNotebookGuid(generatedNotebook);
    }
    note->setEnmlContent(content.enml());
    note->setCreated(QDateTime::currentDateTime());
    note->setUpdated(note->created());

    beginInsertRows(QModelIndex(), m_notes.count(), m_notes.count());
    m_notesHash.insert(note->guid(), note);
    m_notes.append(note);
    endInsertRows();

    emit countChanged();
    emit noteAdded(note->guid(), note->notebookGuid());
    emit noteCreated(note->guid(), note->notebookGuid());

    syncToCacheFile(note);

    if (EvernoteConnection::instance()->isConnected()) {
        CreateNoteJob *job = new CreateNoteJob(note);
        connect(job, &CreateNoteJob::jobDone, this, &NotesStore::createNoteJobDone);
        EvernoteConnection::instance()->enqueue(job);
    }
    return note;
}

void NotesStore::createNoteJobDone(EvernoteConnection::ErrorCode errorCode, const QString &errorMessage, const QString &tmpGuid, const evernote::edam::Note &result)
{
    Note *note = m_notesHash.value(tmpGuid);
    if (!note) {
        qCWarning(dcSync) << "Cannot find temporary note after create operation!";
        return;
    }
    int idx = m_notes.indexOf(note);
    QVector<int> roles;

    note->setLoading(false);
    roles << RoleLoading;

    handleUserError(errorCode);
    if (errorCode != EvernoteConnection::ErrorCodeNoError) {
        qCWarning(dcSync) << "Error creating note on server:" << tmpGuid << errorMessage;
        note->setSyncError(true);
        roles << RoleSyncError;
        emit dataChanged(index(idx), index(idx), roles);
        return;
    }

    if (note->syncError()) {
        note->setSyncError(false);
        roles << RoleSyncError;
    }

    QString guid = QString::fromStdString(result.guid);
    qCDebug(dcSync) << "Note created on server. Old guid:" << tmpGuid << "New guid:" << guid;
    m_notesHash.insert(guid, note);
    note->setGuid(guid);
    m_notesHash.remove(tmpGuid);
    emit noteGuidChanged(tmpGuid, guid);
    roles << RoleGuid;

    if (note->updateSequenceNumber() != result.updateSequenceNum) {
        note->setUpdateSequenceNumber(result.updateSequenceNum);
        note->setLastSyncedSequenceNumber(result.updateSequenceNum);
        roles << RoleSynced;
    }
    if (result.__isset.created) {
        note->setCreated(QDateTime::fromMSecsSinceEpoch(result.created));
        roles << RoleCreated;
    }
    if (result.__isset.updated) {
        note->setUpdated(QDateTime::fromMSecsSinceEpoch(result.updated));
        roles << RoleUpdated;
    }
    if (result.__isset.notebookGuid) {
        note->setNotebookGuid(QString::fromStdString(result.notebookGuid));
        roles << RoleNotebookGuid;
    }
    if (result.__isset.title) {
        note->setTitle(QString::fromStdString(result.title));
        roles << RoleTitle;
    }
    if (result.__isset.content) {
        note->setEnmlContent(QString::fromStdString(result.content));
        roles << RoleEnmlContent << RoleRichTextContent << RoleTagline << RolePlaintextContent;
    }
    emit dataChanged(index(idx), index(idx), roles);

    QSettings cacheFile(m_cacheFile, QSettings::IniFormat);
    cacheFile.beginGroup("notes");
    cacheFile.remove(tmpGuid);
    cacheFile.endGroup();

    syncToCacheFile(note);
}

void NotesStore::saveNote(const QString &guid)
{
    Note *note = m_notesHash.value(guid);
    if (!note) {
        qCWarning(dcNotesStore) << "Can't save note. Guid not found:" << guid;
        return;
    }
    qCDebug(dcNotesStore) << "Saving note. Setting updateSequenceNumber to:" << note->updateSequenceNumber()+1;
    note->setUpdateSequenceNumber(note->updateSequenceNumber()+1);
    note->setUpdated(QDateTime::currentDateTime());
    syncToCacheFile(note);
    note->syncToCacheFile();

    if (EvernoteConnection::instance()->isConnected()) {
        note->setLoading(true);
        if (note->lastSyncedSequenceNumber() == 0) {
            // This note hasn't been created on the server yet... try that first
            CreateNoteJob *job = new CreateNoteJob(note, this);
            connect(job, &CreateNoteJob::jobDone, this, &NotesStore::createNoteJobDone);
            EvernoteConnection::instance()->enqueueWrite(job);
        } else {
            SaveNoteJob *job = new SaveNoteJob(note, this);
            connect(job, &SaveNoteJob::jobDone, this, &NotesStore::saveNoteJobDone);
            EvernoteConnection::instance()->enqueueWrite(job);
        }
    }

    int idx = m_notes.indexOf(note);
    emit dataChanged(index(idx), index(idx));
    emit noteChanged(guid, note->notebookGuid());

    m_organizerAdapter->startSync();
}

void NotesStore::saveNoteJobDone(EvernoteConnection::ErrorCode errorCode, const QString &errorMessage, const evernote::edam::Note &result)
{
    qCDebug(dcSync) << "Note saved to server:" << QString::fromStdString(result.guid);
    Note *note = m_notesHash.value(QString::fromStdString(result.guid));
    if (!note) {
        qCWarning(dcSync) << "Got a save note job result, but note has disappeared locally.";
        return;
    }

    int idx = m_notes.indexOf(note);
    note->setLoading(false);
    QModelIndex noteIndex = index(idx);

    handleUserError(errorCode);
    if (errorCode != EvernoteConnection::ErrorCodeNoError) {
        qCWarning(dcSync) << "Unhandled error saving note:" << errorCode << "Message:" << errorMessage;
        note->setSyncError(true);
        emit dataChanged(noteIndex, noteIndex, QVector<int>() << RoleLoading << RoleSyncError);
        return;
    }

    note->setLastSyncedSequenceNumber(result.updateSequenceNum);
    syncToCacheFile(note);

    emit dataChanged(noteIndex, noteIndex);
    emit noteChanged(note->guid(), note->notebookGuid());
}

void NotesStore::saveNotebookJobDone(EvernoteConnection::ErrorCode errorCode, const QString &errorMessage, const evernote::edam::Notebook &result)
{
    Notebook *notebook = m_notebooksHash.value(QString::fromStdString(result.guid));
    if (!notebook) {
        qCWarning(dcSync) << "Save notebook job done but notebook can't be found any more!";
        return;
    }

    handleUserError(errorCode);
    if (errorCode != EvernoteConnection::ErrorCodeNoError) {
        qCWarning(dcSync) << "Error saving notebook to server" << errorCode << errorMessage;
        notebook->setSyncError(true);
        emit notebookChanged(notebook->guid());
        return;
    }

    notebook->setLoading(false);
    notebook->setSyncError(false);

    qCDebug(dcSync) << "Notebooks saved to server:" << notebook->guid();
    updateFromEDAM(result, notebook);
    emit notebookChanged(notebook->guid());
    syncToCacheFile(notebook);
}

void NotesStore::deleteNote(const QString &guid)
{
    Note *note = m_notesHash.value(guid);
    if (!note) {
        qCWarning(dcNotesStore) << "Note not found. Can't delete";
        return;
    }

    int idx = m_notes.indexOf(note);

    if (note->lastSyncedSequenceNumber() == 0) {
        removeNote(guid);
    } else {

        qCDebug(dcNotesStore) << "Setting note to deleted:" << note->guid();
        note->setDeleted(true);
        note->setUpdateSequenceNumber(note->updateSequenceNumber()+1);
        emit dataChanged(index(idx), index(idx), QVector<int>() << RoleDeleted);

        syncToCacheFile(note);
        if (EvernoteConnection::instance()->isConnected()) {
            DeleteNoteJob *job = new DeleteNoteJob(guid, this);
            connect(job, &DeleteNoteJob::jobDone, this, &NotesStore::deleteNoteJobDone);
            EvernoteConnection::instance()->enqueue(job);
        }
    }

    if (note->reminder() && !note->reminderDone()) {
        m_organizerAdapter->startSync();
    }
}

void NotesStore::findNotes(const QString &searchWords)
{
    if (EvernoteConnection::instance()->isConnected()) {
        clearSearchResults();
        FetchNotesJob *job = new FetchNotesJob(QString(), searchWords + "*");
        connect(job, &FetchNotesJob::jobDone, this, &NotesStore::fetchNotesJobDone);
        EvernoteConnection::instance()->enqueue(job);
    } else {
        foreach (Note *note, m_notes) {
            bool matches = note->title().contains(searchWords, Qt::CaseInsensitive);
            matches |= note->plaintextContent().contains(searchWords, Qt::CaseInsensitive);
            note->setIsSearchResult(matches);
        }
        emit dataChanged(index(0), index(m_notes.count()-1), QVector<int>() << RoleIsSearchResult);
    }
}

void NotesStore::clearSearchResults()
{
    foreach (Note *note, m_notes) {
        note->setIsSearchResult(false);
    }
    emit dataChanged(index(0), index(m_notes.count()-1), QVector<int>() << RoleIsSearchResult);
}

void NotesStore::deleteNoteJobDone(EvernoteConnection::ErrorCode errorCode, const QString &errorMessage, const QString &guid)
{
    handleUserError(errorCode);
    if (errorCode != EvernoteConnection::ErrorCodeNoError) {
        qCWarning(dcSync) << "Cannot delete note from server:" << errorMessage;
        return;
    }
    removeNote(guid);
}

void NotesStore::expungeNotebookJobDone(EvernoteConnection::ErrorCode errorCode, const QString &errorMessage, const QString &guid)
{
    handleUserError(errorCode);
    if (errorCode != EvernoteConnection::ErrorCodeNoError) {
        qCWarning(dcSync) << "Error expunging notebook:" << errorMessage;
        return;
    }

    if (!m_notebooksHash.contains(guid)) {
        qCWarning(dcSync) << "Received a response for a expungeNotebook call, but can't find notebook around any more.";
        return;
    }

    emit notebookRemoved(guid);
    Notebook *notebook = m_notebooksHash.take(guid);
    m_notebooks.removeAll(notebook);

    QSettings settings(m_cacheFile, QSettings::IniFormat);
    settings.beginGroup("notebooks");
    settings.remove(notebook->guid());
    settings.endGroup();

    notebook->deleteInfoFile();
    notebook->deleteLater();
}

void NotesStore::emitDataChanged()
{
    Note *note = qobject_cast<Note*>(sender());
    if (!note) {
        return;
    }
    int idx = m_notes.indexOf(note);
    emit dataChanged(index(idx), index(idx));
}

void NotesStore::clear()
{
    beginResetModel();
    foreach (Note *note, m_notes) {
        emit noteRemoved(note->guid(), note->notebookGuid());
        note->deleteLater();
    }
    m_notes.clear();
    m_notesHash.clear();
    endResetModel();

    while (!m_notebooks.isEmpty()) {
        Notebook *notebook = m_notebooks.takeFirst();
        m_notebooksHash.remove(notebook->guid());
        emit notebookRemoved(notebook->guid());
    }

    while (!m_tags.isEmpty()) {
        Tag *tag = m_tags.takeFirst();
        m_tagsHash.remove(tag->guid());
        emit tagRemoved(tag->guid());
    }
}

void NotesStore::syncToCacheFile(Note *note)
{
    qCDebug(dcNotesStore) << "Syncing note to disk:" << note->guid();
    QSettings cacheFile(m_cacheFile, QSettings::IniFormat);
    cacheFile.beginGroup("notes");
    cacheFile.setValue(note->guid(), note->updateSequenceNumber());
    cacheFile.endGroup();
    note->syncToInfoFile();
}

void NotesStore::deleteFromCacheFile(Note *note)
{
    QSettings cacheFile(m_cacheFile, QSettings::IniFormat);
    cacheFile.beginGroup("notes");
    cacheFile.remove(note->guid());
    cacheFile.endGroup();
    note->deleteFromCache();
}

void NotesStore::syncToCacheFile(Notebook *notebook)
{
    QSettings cacheFile(m_cacheFile, QSettings::IniFormat);
    cacheFile.beginGroup("notebooks");
    cacheFile.setValue(notebook->guid(), notebook->updateSequenceNumber());
    cacheFile.endGroup();
    notebook->syncToInfoFile();
}

void NotesStore::syncToCacheFile(Tag *tag)
{
    QSettings cacheFile(m_cacheFile, QSettings::IniFormat);
    cacheFile.beginGroup("tags");
    cacheFile.setValue(tag->guid(), tag->updateSequenceNumber());
    cacheFile.endGroup();
    tag->syncToInfoFile();
}

void NotesStore::loadFromCacheFile()
{
    clear();
    QSettings cacheFile(m_cacheFile, QSettings::IniFormat);

    cacheFile.beginGroup("notebooks");
    if (cacheFile.allKeys().count() > 0) {
        foreach (const QString &key, cacheFile.allKeys()) {
            Notebook *notebook = new Notebook(key, cacheFile.value(key).toUInt(), this);
            m_notebooksHash.insert(key, notebook);
            m_notebooks.append(notebook);
            emit notebookAdded(key);
        }
    }
    cacheFile.endGroup();
    qCDebug(dcNotesStore) << "Loaded" << m_notebooks.count() << "notebooks from disk.";

    cacheFile.beginGroup("tags");
    if (cacheFile.allKeys().count() > 0) {
        foreach (const QString &key, cacheFile.allKeys()) {
            Tag *tag = new Tag(key, cacheFile.value(key).toUInt(), this);
            m_tagsHash.insert(key, tag);
            m_tags.append(tag);
            emit tagAdded(key);
        }
    }
    cacheFile.endGroup();
    qCDebug(dcNotesStore) << "Loaded" << m_tags.count() << "tags from disk.";

    cacheFile.beginGroup("notes");
    if (cacheFile.allKeys().count() > 0) {
        beginInsertRows(QModelIndex(), 0, cacheFile.allKeys().count()-1);
        foreach (const QString &key, cacheFile.allKeys()) {
            if (m_notesHash.contains(key)) {
                qCWarning(dcNotesStore) << "already have note. Not reloading from cache.";
                continue;
            }
            Note *note = new Note(key, cacheFile.value(key).toUInt(), this);
            m_notesHash.insert(key, note);
            m_notes.append(note);
            emit noteAdded(note->guid(), note->notebookGuid());
        }
        endInsertRows();
    }
    cacheFile.endGroup();
    qCDebug(dcNotesStore) << "Loaded" << m_notes.count() << "notes from disk.";
}

QVector<int> NotesStore::updateFromEDAM(const evernote::edam::NoteMetadata &evNote, Note *note)
{
    QVector<int> roles;
    if (note->guid() != QString::fromStdString(evNote.guid)) {
        note->setGuid(QString::fromStdString(evNote.guid));
        roles << RoleGuid;
    }

    if (evNote.__isset.title && note->title() != QString::fromStdString(evNote.title)) {
        note->setTitle(QString::fromStdString(evNote.title));
        roles << RoleTitle;
    }
    if (evNote.__isset.created && note->created() != QDateTime::fromMSecsSinceEpoch(evNote.created)) {
        note->setCreated(QDateTime::fromMSecsSinceEpoch(evNote.created));
        roles << RoleCreated;
    }
    if (evNote.__isset.updated && note->updated() != QDateTime::fromMSecsSinceEpoch(evNote.updated)) {
        note->setUpdated(QDateTime::fromMSecsSinceEpoch(evNote.updated));
        roles << RoleUpdated;
    }
    if (evNote.__isset.updateSequenceNum && note->updateSequenceNumber() != evNote.updateSequenceNum) {
        note->setUpdateSequenceNumber(evNote.updateSequenceNum);
    }
    if (evNote.__isset.notebookGuid && note->notebookGuid() != QString::fromStdString(evNote.notebookGuid)) {
        note->setNotebookGuid(QString::fromStdString(evNote.notebookGuid));
        roles << RoleNotebookGuid;
    }
    if (evNote.__isset.tagGuids) {
        QStringList tagGuids;
        for (quint32 i = 0; i < evNote.tagGuids.size(); i++) {
            tagGuids << QString::fromStdString(evNote.tagGuids.at(i));
        }
        if (note->tagGuids() != tagGuids) {
            note->setTagGuids(tagGuids);
            roles << RoleTagGuids;
        }
    }
    if (evNote.__isset.attributes && evNote.attributes.__isset.reminderOrder) {
        note->setReminderOrder(evNote.attributes.reminderOrder);
        roles << RoleReminder;
    }
    if (evNote.__isset.attributes && evNote.attributes.__isset.reminderTime) {
        QDateTime reminderTime;
        if (evNote.attributes.reminderTime > 0) {
            reminderTime = QDateTime::fromMSecsSinceEpoch(evNote.attributes.reminderTime);
        }
        if (note->reminderTime() != reminderTime) {
            note->setReminderTime(reminderTime);
            roles << RoleReminderTime;
        }
    }
    if (evNote.__isset.attributes && evNote.attributes.__isset.reminderDoneTime) {
        QDateTime reminderDoneTime;
        if (evNote.attributes.reminderDoneTime > 0) {
            reminderDoneTime = QDateTime::fromMSecsSinceEpoch(evNote.attributes.reminderDoneTime);
        }
        if (note->reminderDoneTime() != reminderDoneTime) {
            note->setReminderDoneTime(reminderDoneTime);
            roles << RoleReminderDoneTime;
        }
    }
    if (evNote.__isset.deleted) {
        note->setDeleted(evNote.deleted);
        roles << RoleDeleted;
    }
    note->setLastSyncedSequenceNumber(evNote.updateSequenceNum);
    return roles;
}

void NotesStore::updateFromEDAM(const evernote::edam::Notebook &evNotebook, Notebook *notebook)
{
    if (evNotebook.__isset.guid && QString::fromStdString(evNotebook.guid) != notebook->guid()) {
        notebook->setGuid(QString::fromStdString(evNotebook.guid));
    }
    if (evNotebook.__isset.name && QString::fromStdString(evNotebook.name) != notebook->name()) {
        notebook->setName(QString::fromStdString(evNotebook.name));
    }
    if (evNotebook.__isset.updateSequenceNum && evNotebook.updateSequenceNum != notebook->updateSequenceNumber()) {
        notebook->setUpdateSequenceNumber(evNotebook.updateSequenceNum);
    }
    if (evNotebook.__isset.serviceUpdated && QDateTime::fromMSecsSinceEpoch(evNotebook.serviceUpdated) != notebook->lastUpdated()) {
        notebook->setLastUpdated(QDateTime::fromMSecsSinceEpoch(evNotebook.serviceUpdated));
    }
    if (evNotebook.__isset.published && evNotebook.published != notebook->published()) {
        notebook->setPublished(evNotebook.published);
    }
    if (evNotebook.__isset.defaultNotebook && evNotebook.defaultNotebook != notebook->isDefaultNotebook()) {
        notebook->setIsDefaultNotebook(evNotebook.defaultNotebook);
    }
    notebook->setLastSyncedSequenceNumber(evNotebook.updateSequenceNum);
}

bool NotesStore::handleUserError(EvernoteConnection::ErrorCode errorCode)
{
    switch (errorCode) {
    case EvernoteConnection::ErrorCodeAuthExpired:
        m_errorQueue.append(gettext("Authentication for Evernote server expired. Please renew login information in the accounts settings."));
        break;
    case EvernoteConnection::ErrorCodeLimitExceeded:
        m_errorQueue.append(gettext("Rate limit for Evernote server exceeded. Please try again later."));
        break;
    case EvernoteConnection::ErrorCodeQutaExceeded:
        m_errorQueue.append(gettext("Upload quota for Evernote server exceed. Please try again later."));
        break;
    default:
        return false;
    }
    emit errorChanged();
    return true;
}

void NotesStore::removeNote(const QString &guid)
{
    Note *note = m_notesHash.value(guid);
    int idx = m_notes.indexOf(note);

    emit noteRemoved(note->guid(), note->notebookGuid());

    beginRemoveRows(QModelIndex(), idx, idx);
    m_notes.removeAt(idx);
    m_notesHash.remove(note->guid());
    endRemoveRows();
    emit countChanged();

    QSettings settings(m_cacheFile, QSettings::IniFormat);
    settings.beginGroup("notes");
    settings.remove(note->guid());
    settings.endGroup();

    note->deleteLater();
}

void NotesStore::expungeTag(const QString &guid)
{
#ifdef NO_EXPUNGE_TAGS
    // This snipped can be used if the app is compiled with a restricted api key
    // that can't expunge tags on Evernote. Compile with
    // cmake -DNO_EXPUNGE_TAGS=1

    if (m_username != "@local") {
        qCWarning(dcNotesStore) << "This account is managed by Evernote. Cannot delete tags.";
        m_errorQueue.append(gettext("This account is managed by Evernote. Please use the Evernote website to delete tags."));
        emit errorChanged();
        return;
    }
#endif

    Tag *tag = m_tagsHash.value(guid);
    if (!tag) {
        qCWarning(dcNotesStore) << "No tag with guid" << guid;
        return;
    }

    while (tag->noteCount() > 0) {
        QString noteGuid = tag->noteAt(0);
        Note *note = m_notesHash.value(noteGuid);
        if (!note) {
            qCWarning(dcNotesStore) << "Tag holds note" << noteGuid << "which hasn't been found in Notes Store";
            Q_ASSERT(false);
            continue;
        }
        untagNote(noteGuid, guid);
    }

    if (tag->lastSyncedSequenceNumber() == 0) {
        emit tagRemoved(guid);
        m_tagsHash.remove(guid);
        m_tags.removeAll(tag);

        QSettings cacheFile(m_cacheFile, QSettings::IniFormat);
        cacheFile.beginGroup("tags");
        cacheFile.remove(guid);
        cacheFile.endGroup();
        tag->syncToInfoFile();

        tag->deleteInfoFile();
        tag->deleteLater();
    } else {
        qCDebug(dcNotesStore) << "Setting tag to deleted:" << tag->guid();
        tag->setDeleted(true);
        tag->setUpdateSequenceNumber(tag->updateSequenceNumber()+1);
        emit tagChanged(tag->guid());
        syncToCacheFile(tag);

        if (EvernoteConnection::instance()->isConnected()) {
            ExpungeTagJob *job = new ExpungeTagJob(guid, this);
            connect(job, &ExpungeTagJob::jobDone, this, &NotesStore::expungeTagJobDone);
            EvernoteConnection::instance()->enqueue(job);
        }
    }
}

void NotesStore::resolveConflict(const QString &noteGuid, NotesStore::ConflictResolveMode mode)
{
    Note *note = m_notesHash.value(noteGuid);
    if (!note) {
        qCWarning(dcNotesStore) << "Should resolve a conflict but can't find note for guid:" << noteGuid;
        return;
    }
    if (!note->conflictingNote()) {
        qCWarning(dcNotesStore) << "Should resolve a conflict but note doesn't have a conflicting note:" << noteGuid;
        return;
    }
    if (mode == KeepLocal) {
        qCDebug(dcNotesStore) << "Resolving conflict using local note for note guid:" << noteGuid;
        note->setUpdateSequenceNumber(note->conflictingNote()->updateSequenceNumber() + 1);
        note->setConflicting(false);
        saveNote(note->guid());
    } else {
        qCDebug(dcNotesStore) << "Resolving conflict using remote note for note guid:" << noteGuid;
        Note *newNote = note->conflictingNote();
        newNote->setParent(this);
        // Conflicting notes have their guid prefixed, lets correct that
        newNote->setGuid(note->guid());
        newNote->setConflicting(false);
        int idx = m_notes.indexOf(note);
        m_notesHash[note->guid()] = newNote;
        m_notes.replace(idx, newNote);
        emit noteChanged(newNote->guid(), newNote->notebookGuid());
        emit dataChanged(index(idx), index(idx));
        saveNote(note->guid());
    }
}
