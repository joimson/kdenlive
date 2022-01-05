/*
    SPDX-FileCopyrightText: 2017 Nicolas Carion
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "meltBuilder.hpp"
#include "../clipmodel.hpp"
#include "../timelineitemmodel.hpp"
#include "../timelinemodel.hpp"
#include "../trackmodel.hpp"
#include "../undohelper.hpp"
#include "bin/bin.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "kdenlivesettings.h"

#include <KLocalizedString>
#include <KMessageBox>
#include <QDebug>
#include <QProgressDialog>
#include <QSet>
#include <mlt++/MltPlaylist.h>
#include <mlt++/MltProducer.h>
#include <mlt++/MltProfile.h>
#include <mlt++/MltFilter.h>
#include <mlt++/MltField.h>
#include <mlt++/MltTransition.h>
#include <QApplication>
#include <project/projectmanager.h>

static QStringList m_errorMessage;
static QStringList m_notesLog;

bool constructTrackFromMelt(const std::shared_ptr<TimelineItemModel> &timeline, int tid, Mlt::Tractor &track,
                            const std::unordered_map<QString, QString> &binIdCorresp, Fun &undo, Fun &redo, bool audioTrack, const QString &originalDecimalPoint, QProgressDialog *progressDialog = nullptr);
bool constructTrackFromMelt(const std::shared_ptr<TimelineItemModel> &timeline, int tid, Mlt::Playlist &track,
                            const std::unordered_map<QString, QString> &binIdCorresp, Fun &undo, Fun &redo, bool audioTrack, const QString &originalDecimalPoint, int playlist, const QList<Mlt::Transition *> &compositions, QProgressDialog *progressDialog = nullptr);

bool constructTimelineFromMelt(const std::shared_ptr<TimelineItemModel> &timeline, Mlt::Tractor tractor, QProgressDialog *progressDialog, const QString &originalDecimalPoint, bool *projectErrors)
{
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    // First, we destruct the previous tracks
    timeline->requestReset(undo, redo);
    m_errorMessage.clear();
    m_notesLog.clear();
    std::unordered_map<QString, QString> binIdCorresp;
    QStringList expandedFolders;
    pCore->projectItemModel()->loadBinPlaylist(&tractor, timeline->tractor(), binIdCorresp, expandedFolders, progressDialog);
    QStringList foldersToExpand;
    // Find updated ids for expanded folders
    for (const QString &folderId : expandedFolders) {
        if (binIdCorresp.count(folderId) > 0) {
            foldersToExpand << binIdCorresp.at(folderId);
        }
    }
    if (pCore->window()) {
        pCore->bin()->checkMissingProxies();
        pCore->bin()->loadFolderState(foldersToExpand);
    }

    QSet<QString> reserved_names{QLatin1String("playlistmain"), QLatin1String("timeline_preview"), QLatin1String("timeline_overlay"), QLatin1String("black_track"), QLatin1String("overlay_track")};
    bool ok = true;

    // Import master track effects
    std::shared_ptr<Mlt::Service> serv = std::make_shared<Mlt::Service>(tractor.get_service());
    timeline->importMasterEffects(serv);

    QList <int> videoTracksIndexes;
    QList <int> lockedTracksIndexes;
    // Black track index
    videoTracksIndexes << 0;
    for (int i = 0; i < tractor.count() && ok; i++) {
        std::unique_ptr<Mlt::Producer> track(tractor.track(i));
        QString playlist_name = track->get("id");
        if (reserved_names.contains(playlist_name)) {
            continue;
        }
        switch (track->type()) {
        case mlt_service_producer_type:
            // TODO check that it is the black track, and otherwise log an error
            break;
        case mlt_service_tractor_type: {
            // that is a double track
            int tid;
            bool audioTrack = track->get_int("kdenlive:audio_track") == 1;
            if (!audioTrack) {
                videoTracksIndexes << i;
            }
            ok = timeline->requestTrackInsertion(-1, tid, QString(), audioTrack, undo, redo, false);
            if (track->get_int("kdenlive:locked_track") > 0) {
                lockedTracksIndexes << tid;
            }
            Mlt::Tractor local_tractor(*track);
            ok = ok && constructTrackFromMelt(timeline, tid, local_tractor, binIdCorresp, undo, redo, audioTrack, originalDecimalPoint, progressDialog);
            timeline->setTrackProperty(tid, QStringLiteral("kdenlive:thumbs_format"), track->get("kdenlive:thumbs_format"));
            timeline->setTrackProperty(tid, QStringLiteral("kdenlive:audio_rec"), track->get("kdenlive:audio_rec"));
            timeline->setTrackProperty(tid, QStringLiteral("kdenlive:timeline_active"), track->get("kdenlive:timeline_active"));
            break;
        }
        case mlt_service_playlist_type: {
            // that is a single track
            int tid;
            Mlt::Playlist local_playlist(*track);
            const QString trackName = local_playlist.get("kdenlive:track_name");
            bool audioTrack = local_playlist.get_int("kdenlive:audio_track") == 1;
            if (!audioTrack) {
                videoTracksIndexes << i;
            }
            ok = timeline->requestTrackInsertion(-1, tid, trackName, audioTrack, undo, redo, false);
            int muteState = track->get_int("hide");
            if (muteState > 0 && (!audioTrack || (audioTrack && muteState != 1))) {
                timeline->setTrackProperty(tid, QStringLiteral("hide"), QString::number(muteState));
            }

            ok = ok && constructTrackFromMelt(timeline, tid, local_playlist, binIdCorresp, undo, redo, audioTrack, originalDecimalPoint, 0, QList<Mlt::Transition *> (), progressDialog);
            if (local_playlist.get_int("kdenlive:locked_track") > 0) {
                lockedTracksIndexes << tid;
            }
            timeline->setTrackProperty(tid, QStringLiteral("kdenlive:thumbs_format"), local_playlist.get("kdenlive:thumbs_format"));
            timeline->setTrackProperty(tid, QStringLiteral("kdenlive:audio_rec"), track->get("kdenlive:audio_rec"));
            timeline->setTrackProperty(tid, QStringLiteral("kdenlive:timeline_active"), track->get("kdenlive:timeline_active"));
            break;
        }
        default:
            qWarning() << "Unexpected track type" << track->type();
        }
    }
    timeline->_resetView();

    // Loading compositions
    QScopedPointer<Mlt::Service> service(tractor.producer());
    QList<Mlt::Transition *> compositions;
    while ((service != nullptr) && service->is_valid()) {
        if (service->type() == mlt_service_transition_type) {
            Mlt::Transition t(mlt_transition(service->get_service()));
            if (t.get_b_track() >= timeline->tractor()->count()) {
                // Composition outside of available track, maybe because of a preview track
                service.reset(service->producer());
                continue;
            }
            QString id(t.get("kdenlive_id"));
            if (t.property_exists("internal_added") == false && t.property_exists("kdenlive:mixcut") == false) {
                compositions << new Mlt::Transition(t);
                if (id.isEmpty()) {
                    qWarning() << "transition without id" << t.get("id") << t.get("mlt_service") << "on track" << t.get_b_track();
                    t.set("kdenlive_id", t.get("mlt_service"));
                }
            }
        }
        service.reset(service->producer());
    }
    // Sort compositions and insert
    bool compositionOk = true;
    while (!compositions.isEmpty()) {
        QScopedPointer<Mlt::Transition> t(compositions.takeFirst());
        QString id(t->get("kdenlive_id"));
        int compoId;
        int aTrack = t->get_a_track();
        if (!timeline->isTrack(timeline->getTrackIndexFromPosition(t->get_b_track() - 1))) {
            QString tcInfo = QString("<a href=\"%1\">%2</a>").arg(QString::number(t->get_in()), pCore->timecode().getTimecodeFromFrames(t->get_in()));
            m_notesLog << i18n("%1 Composition (%2) with invalid track reference found and removed.", tcInfo, t->get("id"));
            m_errorMessage << i18n("Invalid composition %1 found on track %2 at %3, compositing with track %4.", t->get("id"), t->get_b_track(),
                                       t->get_in(), t->get_a_track());
            continue;
        }
        if (aTrack > tractor.count()) {
            int tid = timeline->getTrackIndexFromPosition(t->get_b_track() - 1);
            QString tcInfo = QString("<a href=\"%1?%2\">%3 %4</a>").arg(QString::number(t->get_in()), QString::number(timeline->getTrackPosition(tid)+1), timeline->getTrackTagById(tid), pCore->timecode().getTimecodeFromFrames(t->get_in()));
            m_notesLog << i18n("%1 Composition (%2) with invalid track reference found and removed.", tcInfo, t->get("id"));
            m_errorMessage << i18n("Invalid composition %1 found on track %2 at %3, compositing with track %4.", t->get("id"), t->get_b_track(),
                                       t->get_in(), t->get_a_track());
            continue;
        }
        if (t->get_int("force_track") == 0) {
            // This is an automatic composition, check that we composite with lower track or warn
            int pos = videoTracksIndexes.indexOf(t->get_b_track());
            if (pos > 0 && videoTracksIndexes.at(pos - 1) != aTrack) {
                t->set("force_track", 1);
                int tid = timeline->getTrackIndexFromPosition(t->get_b_track() - 1);
                QString tcInfo = QString("<a href=\"%1?%2\">%3 %4</a>").arg(QString::number(t->get_in()), QString::number(timeline->getTrackPosition(tid)+1), timeline->getTrackTagById(tid), pCore->timecode().getTimecodeFromFrames(t->get_in()));
                m_notesLog << i18n("%1 Composition was not applied on expected track, manually enforce the track.", tcInfo);
                m_errorMessage << i18n("Incorrect composition %1 found on track %2 at %3, compositing with track %4 was set to forced track.", t->get("id"), t->get_b_track(),
                                    t->get_in(), t->get_a_track());
            }
        }
        auto transProps = std::make_unique<Mlt::Properties>(t->get_properties());
        compositionOk = timeline->requestCompositionInsertion(id, timeline->getTrackIndexFromPosition(t->get_b_track() - 1), t->get_a_track(), t->get_in(), t->get_length(), std::move(transProps), compoId, undo, redo, false, originalDecimalPoint);
        if (!compositionOk) {
            // timeline->requestItemDeletion(compoId, false);
            int tid = timeline->getTrackIndexFromPosition(t->get_b_track() - 1);
            QString tcInfo = QString("<a href=\"%1?%2\">%3 %4</a>").arg(QString::number(t->get_in()), QString::number(timeline->getTrackPosition(tid)+1), timeline->getTrackTagById(tid), pCore->timecode().getTimecodeFromFrames(t->get_in()));
            m_notesLog << i18n("%1 Invalid composition found and removed.", tcInfo);
            m_errorMessage << i18n("Invalid composition %1 found on track %2 at %3.", t->get("id"), t->get_b_track(), t->get_in());
            continue;
        }
    }
    qDeleteAll(compositions);

    // build internal track compositing
    timeline->buildTrackCompositing();

    // load locked state as last step
    for (int tid : qAsConst(lockedTracksIndexes)) {
        timeline->setTrackLockedState(tid, true);
    }

    if (!ok) {
        // TODO log error
        // Don't abort loading because of failed composition
        undo();
        return false;
    }
    if (!m_notesLog.isEmpty()) {
        m_notesLog.prepend(i18n("Errors found when opening project file (%1)", QDateTime::currentDateTime().toString()));
        pCore->projectManager()->slotAddTextNote(m_notesLog.join("<br/>"));
        if (projectErrors) {
            *projectErrors = true;
        }
        KMessageBox::detailedSorry(qApp->activeWindow(), i18n("Some errors were detected in the project file.\nThe project was modified to fix the conflicts. Changes made to the project have been listed in the Project Notes tab,\nplease review them to ensure your project integrity."), m_errorMessage.join("\n"), i18n("Problems found in your project file"));
    } else if (!m_errorMessage.isEmpty()) {
        KMessageBox::sorry(qApp->activeWindow(), m_errorMessage.join("\n"), i18n("Problems found in your project file"));
    }
    return true;
}

bool constructTrackFromMelt(const std::shared_ptr<TimelineItemModel> &timeline, int tid, Mlt::Tractor &track,
                            const std::unordered_map<QString, QString> &binIdCorresp, Fun &undo, Fun &redo, bool audioTrack, const QString &originalDecimalPoint, QProgressDialog *progressDialog)
{
    if (track.count() != 2) {
        // we expect a tractor with two tracks (a "fake" track)
        qWarning() << "wrong number of subtracks";
        return false;
    }
    // Check same track transitions
    QScopedPointer<Mlt::Service> service(track.field());
    QList<Mlt::Transition *> compositions;
    while ((service != nullptr) && service->is_valid()) {
        if (service->type() == mlt_service_transition_type) {
            Mlt::Transition t(mlt_transition(service->get_service()));
            QString id(t.get("kdenlive_id"));
            compositions << new Mlt::Transition(t);
            if (id.isEmpty()) {
                qWarning() << "transition without id" << t.get("id") << t.get("mlt_service");
                t.set("kdenlive_id", t.get("mlt_service"));
            }
        }
        service.reset(service->producer());
    }
    for (int i = 0; i < track.count(); i++) {
        std::unique_ptr<Mlt::Producer> sub_track(track.track(i));
        if (sub_track->type() != mlt_service_playlist_type) {
            qWarning() << "subtrack must be playlist";
            return false;
        }
        Mlt::Playlist playlist(*sub_track);
        constructTrackFromMelt(timeline, tid, playlist, binIdCorresp, undo, redo, audioTrack, originalDecimalPoint, i, compositions, progressDialog);
        if (i == 0) {
            // Pass track properties
            int height = track.get_int("kdenlive:trackheight");
            timeline->setTrackProperty(tid, "kdenlive:trackheight", height == 0 ? "100" : QString::number(height));
            timeline->setTrackProperty(tid, "kdenlive:collapsed", QString::number(track.get_int("kdenlive:collapsed")));
            QString trackName = track.get("kdenlive:track_name");
            if (!trackName.isEmpty()) {
                timeline->setTrackProperty(tid, QStringLiteral("kdenlive:track_name"), trackName.toUtf8().constData());
            }
            if (audioTrack) {
                // This is an audio track
                timeline->setTrackProperty(tid, QStringLiteral("kdenlive:audio_track"), QStringLiteral("1"));
                timeline->setTrackProperty(tid, QStringLiteral("hide"), QStringLiteral("1"));
            } else {
                // video track, hide audio
                timeline->setTrackProperty(tid, QStringLiteral("hide"), QStringLiteral("2"));
            }
            int muteState = playlist.get_int("hide");
            if (muteState > 0 && (!audioTrack || (audioTrack && muteState != 1))) {
                timeline->setTrackProperty(tid, QStringLiteral("hide"), QString::number(muteState));
            }
        }
    }
    for (auto compo : qAsConst(compositions)) {
        timeline->plantMix(tid, compo);
    }
    std::shared_ptr<Mlt::Service> serv = std::make_shared<Mlt::Service>(track.get_service());
    timeline->importTrackEffects(tid, serv);
    return true;
}

namespace {

// This function tries to recover the state of the producer (audio or video or both)
PlaylistState::ClipState inferState(const std::shared_ptr<Mlt::Producer> &prod, bool audioTrack)
{
    auto getProperty = [prod](const QString &name) {
        if (prod->parent().is_valid()) {
            return QString::fromUtf8(prod->parent().get(name.toUtf8().constData()));
        }
        return QString::fromUtf8(prod->get(name.toUtf8().constData()));
    };
    auto getIntProperty = [prod](const QString &name) {
        if (prod->parent().is_valid()) {
            return prod->parent().get_int(name.toUtf8().constData());
        }
        return prod->get_int(name.toUtf8().constData());
    };
    QString service = getProperty("mlt_service");
    std::pair<bool, bool> VidAud{true, true};
    VidAud.first = getIntProperty("set.test_image") == 0;
    VidAud.second = getIntProperty("set.test_audio") == 0;
    if (audioTrack || ((service.contains(QStringLiteral("avformat")) && getIntProperty(QStringLiteral("video_index")) == -1))) {
        VidAud.first = false;
    }
    if (!audioTrack || ((service.contains(QStringLiteral("avformat")) && getIntProperty(QStringLiteral("audio_index")) == -1))) {
        VidAud.second = false;
    }
    return stateFromBool(VidAud);
}
} // namespace

bool constructTrackFromMelt(const std::shared_ptr<TimelineItemModel> &timeline, int tid, Mlt::Playlist &track,
                            const std::unordered_map<QString, QString> &binIdCorresp, Fun &undo, Fun &redo, bool audioTrack, const QString &originalDecimalPoint, int playlist, const QList<Mlt::Transition *> &compositions, QProgressDialog *progressDialog)
{
    int max = track.count();
    for (int i = 0; i < max; i++) {
        if (track.is_blank(i)) {
            continue;
        }
        if (progressDialog) {
            progressDialog->setValue(progressDialog->value() + 1);
        } else {
            emit pCore->loadingMessageUpdated(QString(), 1);
        }
        std::shared_ptr<Mlt::Producer> clip(track.get_clip(i));
        int position = track.clip_start(i);
        switch (clip->type()) {
        case mlt_service_unknown_type:
        case mlt_service_producer_type: {
            QString binId;
            if (clip->parent().get_int("_kdenlive_processed") == 1) {
                // This is a bin clip, already processed no need to change id
                binId = QString(clip->parent().get("kdenlive:id"));
            } else {
                QString clipId = clip->parent().get("kdenlive:id");
                if (clipId.startsWith(QStringLiteral("slowmotion"))) {
                    clipId = clipId.section(QLatin1Char(':'), 1, 1);
                }
                if (clipId.isEmpty()) {
                    clipId = clip->get("kdenlive:id");
                }
                if (binIdCorresp.count(clipId) == 0) {
                    if (clip->property_exists("kdenlive:remove")) {
                        // Clip was marked for deletion
                        continue;
                    }
                    // Project was somehow corrupted
                    qWarning() << "can't find clip with id: " << clipId << "in bin playlist";
                    QStringList fixedId = pCore->projectItemModel()->getClipByUrl(QFileInfo(clip->parent().get("resource")));
                    QString tcInfo = QString("<a href=\"%1?%2\">%3 %4</a>").arg(QString::number(position), QString::number(timeline->getTrackPosition(tid)+1), timeline->getTrackTagById(tid), pCore->timecode().getTimecodeFromFrames(position));
                    if (!fixedId.isEmpty()) {
                        binId = fixedId.first();
                        m_notesLog << i18n("%1 Timeline clip (%2) with incorrect bin reference found and recovered.", tcInfo, clip->parent().get("id"));
                        m_errorMessage << i18n("Invalid clip %1 (%2) not found in project bin, recovered.", clip->parent().get("id"), clipId);
                    } else {
                        m_notesLog << i18n("%1 Timeline clip (%2) without bin reference found and removed.", tcInfo, clip->parent().get("id"));
                        m_errorMessage << i18n("Project corrupted. Clip %1 (%2) not found in project bin.", clip->parent().get("id"), clipId);
                        // Do not try to insert clip
                        continue;
                    }
                } else {
                    binId = binIdCorresp.at(clipId);
                }
                Q_ASSERT(!clipId.isEmpty() && !binId.isEmpty());
                clip->parent().set("kdenlive:id", binId.toUtf8().constData());
                clip->parent().set("_kdenlive_processed", 1);
            }
            bool ok = false;
            int cid = -1;
            if (pCore->projectItemModel()->getClipByBinID(binId)) {
                PlaylistState::ClipState st = inferState(clip, audioTrack);
                bool enforceTopPlaylist = false;
                if (playlist > 0) {
                    // Clips on playlist > 0 must have a mix or something is wrong
                    bool hasStartMix = !timeline->trackIsBlankAt(tid, position, 0);
                    int duration = clip->get_playtime() - 1;
                    bool hasEndMix = !timeline->trackIsBlankAt(tid, position + duration, 0);
                    bool startMixToFind = hasStartMix;
                    bool endMixToFind = hasEndMix;
                    if (startMixToFind || endMixToFind) {
                        for (auto &compo : compositions) {
                            int in = compo->get_in();
                            int out = compo->get_out();
                            if (startMixToFind && out > position && in <= position) {
                                startMixToFind = false;
                            }
                            if (endMixToFind && in < position + duration && out >= position + duration) {
                                endMixToFind = false;
                            }
                            if (!startMixToFind && !endMixToFind) {
                                break;
                            }
                        }
                        if (startMixToFind || endMixToFind) {
                            // A mix for this clip is missing                           
                            QString tcInfo = QString("<a href=\"%1?%2\">%3 %4</a>").arg(QString::number(position), QString::number(timeline->getTrackPosition(tid)+1), timeline->getTrackTagById(tid), pCore->timecode().getTimecodeFromFrames(position));
                            // Try to resize clip and put it on playlist 0
                            if (hasEndMix && endMixToFind) {
                                // Find last empty frame on playlist 0
                                int lastAvailableFrame = timeline->getClipStartAt(tid, position + duration, 0);
                                if (lastAvailableFrame > position) {
                                    // Resize clip to fit.
                                    int updatedDuration = lastAvailableFrame - position - 1;
                                    int currentIn = clip->get_in();
                                    clip->set_in_and_out(currentIn, currentIn + updatedDuration);
                                    hasEndMix = false;
                                    if (!startMixToFind) {
                                        // Move to top playlist
                                        cid = ClipModel::construct(timeline, binId, clip, st, tid, originalDecimalPoint, hasStartMix ? playlist : 0);
                                        timeline->requestClipMove(cid, tid, position, true, true, false, true, undo, redo);
                                        m_notesLog << i18n("%1 Clip (%2) with missing mix found and resized", tcInfo, clip->parent().get("id"));
                                        m_errorMessage << i18n("Clip without mix %1 found and resized on track %2 at %3.", clip->parent().get("id"), timeline->getTrackTagById(tid), position);
                                        continue;
                                    }
                                } else {
                                    m_errorMessage << i18n("Invalid clip without mix %1 found and removed on track %2 at %3.", clip->parent().get("id"), timeline->getTrackTagById(tid), position);
                                    m_notesLog << i18n("%1 Clip (%2) with missing mix found and removed", tcInfo, clip->parent().get("id"));
                                    continue;
                                }
                            }
                            if (hasStartMix && startMixToFind) {
                                int firstAvailableFrame = timeline->getClipEndAt(tid, position, 0);
                                if (firstAvailableFrame < position + duration) {
                                    int delta = firstAvailableFrame - position;
                                    int currentIn = clip->get_in();
                                    currentIn += delta;
                                    position += delta;
                                    int currentOut = clip->get_out();
                                    clip->set_in_and_out(currentIn, currentOut);
                                    // Move to top playlist
                                    cid = ClipModel::construct(timeline, binId, clip, st, tid, originalDecimalPoint, hasEndMix ? playlist : 0);
                                    ok = timeline->requestClipMove(cid, tid, position, true, true, false, true, undo, redo);
                                    if (!ok && cid > -1) {
                                        timeline->requestItemDeletion(cid, false);
                                        m_errorMessage << i18n("Invalid clip %1 found on track %2 at %3.", clip->parent().get("id"), track.get("id"), position);
                                        m_notesLog << i18n("%1 Invalid clip (%2) found and removed", tcInfo, clip->parent().get("id"));
                                        continue;
                                    }
                                    m_errorMessage << i18n("Clip without mix %1 found and resized on track %2 at %3.", clip->parent().get("id"), timeline->getTrackTagById(tid), position);
                                    m_notesLog << i18n("%1 Clip (%2) with missing mix found and resized", tcInfo, clip->parent().get("id"));
                                    continue;
                                }
                            }
                            m_errorMessage << i18n("Invalid clip without mix %1 found and removed on track %2 at %3.", clip->parent().get("id"), timeline->getTrackTagById(tid), position);
                            m_notesLog << i18n("%1 Clip (%2) with missing mix found and removed", tcInfo, clip->parent().get("id"));
                            continue;
                        }
                    } else {
                        // Check if playlist 0 is available
                        enforceTopPlaylist = timeline->trackIsAvailable(tid, position, duration, 0);
                        if (enforceTopPlaylist) {
                            m_errorMessage << i18n("Clip %1 on incorrect subtrack found and fixed on track %2 at %3.", clip->parent().get("id"), timeline->getTrackTagById(tid), position);
                        } else {
                            m_errorMessage << i18n("Clip %1 on incorrect subtrack found on track %2 at %3.", clip->parent().get("id"), timeline->getTrackTagById(tid), position);
                            QString tcInfo = QString("<a href=\"%1?%2\">%3 %4</a>").arg(QString::number(position), QString::number(timeline->getTrackPosition(tid)+1), timeline->getTrackTagById(tid), pCore->timecode().getTimecodeFromFrames(position));
                            m_notesLog << i18n("%1 Clip (%2) with incorrect subplaylist found", tcInfo, clip->parent().get("id"));
                        }
                    }
                }
                cid = ClipModel::construct(timeline, binId, clip, st, tid, originalDecimalPoint, enforceTopPlaylist ? 0 : playlist);
                ok = timeline->requestClipMove(cid, tid, position, true, true, false, true, undo, redo);
            } else {
                qWarning() << "can't find bin clip" << binId << clip->get("id");
            }
            if (!ok && cid > -1) {
                timeline->requestItemDeletion(cid, false);
                m_errorMessage << i18n("Invalid clip %1 found on track %2 at %3.", clip->parent().get("id"), track.get("id"), position);
                QString tcInfo = QString("<a href=\"%1?%2\">%3 %4</a>").arg(QString::number(position), QString::number(timeline->getTrackPosition(tid)+1), timeline->getTrackTagById(tid), pCore->timecode().getTimecodeFromFrames(position));
                m_notesLog << i18n("%1 Invalid clip (%2) found and removed", tcInfo, clip->parent().get("id"));
                continue;
            }
            break;
        }
        case mlt_service_tractor_type: {
            // TODO This is a nested timeline
            qWarning() << "nested timelines not yet implemented";
            break;
        }
        default:
            qWarning() << "unexpected object found in playlist";
            return false;
            break;
        }
    }
    std::shared_ptr<Mlt::Service> serv = std::make_shared<Mlt::Service>(track.get_service());
    timeline->importTrackEffects(tid, serv);
    return true;
}
