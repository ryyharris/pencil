/*

Pencil - Traditional Animation Software
Copyright (C) 2005-2007 Patrick Corrieri & Pascal Naidon
Copyright (C) 2012-2018 Matthew Chiawen Chang

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

*/

#include "editor.h"

#include <memory>
#include <iostream>
#include <QApplication>
#include <QClipboard>
#include <QTimer>
#include <QImageReader>
#include <QFileDialog>
#include <QInputDialog>
#include <QDragEnterEvent>
#include <QDropEvent>

#include "object.h"
#include "objectdata.h"
#include "vectorimage.h"
#include "bitmapimage.h"
#include "soundclip.h"
#include "layerbitmap.h"
#include "layervector.h"
#include "layercamera.h"
#include "activeframepool.h"

#include "colormanager.h"
#include "toolmanager.h"
#include "layermanager.h"
#include "playbackmanager.h"
#include "viewmanager.h"
#include "preferencemanager.h"
#include "soundmanager.h"
#include "backupmanager.h"

#include "scribblearea.h"
#include "timeline.h"
#include "util.h"
#include "movieexporter.h"

#define MIN(a,b) ((a)>(b)?(b):(a))


static BitmapImage g_clipboardBitmapImage;
static VectorImage g_clipboardVectorImage;


Editor::Editor(QObject* parent) : QObject(parent)
{
    clipboardBitmapOk = false;
    clipboardVectorOk = false;
    clipboardSoundClipOk = false;
}

Editor::~Editor()
{
    // a lot more probably needs to be cleaned here...
}

bool Editor::init()
{
    // Initialize managers
    mColorManager = new ColorManager(this);
    mLayerManager = new LayerManager(this);
    mToolManager = new ToolManager(this);
    mPlaybackManager = new PlaybackManager(this);
    mViewManager = new ViewManager(this);
    mPreferenceManager = new PreferenceManager(this);
    mSoundManager = new SoundManager(this);
    mBackupManager = new BackupManager(this);

    mAllManagers =
    {
        mColorManager,
        mToolManager,
        mLayerManager,
        mPlaybackManager,
        mViewManager,
        mPreferenceManager,
        mSoundManager,
        mBackupManager
    };

    for (BaseManager* pManager : mAllManagers)
    {
        pManager->init();
    }
    //setAcceptDrops( true ); // TODO: drop event

    makeConnections();

    mIsAutosave = mPreferenceManager->isOn(SETTING::AUTO_SAVE);
    mAutosaveNumber = mPreferenceManager->getInt(SETTING::AUTO_SAVE_NUMBER);

    return true;
}

int Editor::currentFrame()
{
    return mFrame;
}

int Editor::fps()
{
    return mPlaybackManager->fps();
}

void Editor::makeConnections()
{
    connect(mPreferenceManager, &PreferenceManager::optionChanged, this, &Editor::settingUpdated);
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this, &Editor::clipboardChanged);
}

void Editor::dragEnterEvent(QDragEnterEvent* event)
{
    event->acceptProposedAction();
}

void Editor::dropEvent(QDropEvent* event)
{
    if (event->mimeData()->hasUrls())
    {
        for (int i = 0; i < event->mimeData()->urls().size(); i++)
        {
            if (i > 0) scrubForward();
            QUrl url = event->mimeData()->urls()[i];
            QString filePath = url.toLocalFile();
            if (filePath.endsWith(".png") || filePath.endsWith(".jpg") || filePath.endsWith(".jpeg"))
            {
                bool isSequence = (i > 1) ? true : false;
                importImage(filePath, isSequence);
            }
            //if ( filePath.endsWith( ".aif" ) || filePath.endsWith( ".mp3" ) || filePath.endsWith( ".wav" ) )
                //importSound( filePath );
        }
    }
}

void Editor::settingUpdated(SETTING setting)
{
    switch (setting)
    {
    case SETTING::AUTO_SAVE:
        mIsAutosave = mPreferenceManager->isOn(SETTING::AUTO_SAVE);
        break;
    case SETTING::AUTO_SAVE_NUMBER:
        mAutosaveNumber = mPreferenceManager->getInt(SETTING::AUTO_SAVE_NUMBER);
        break;
    case SETTING::ONION_TYPE:
        mScribbleArea->updateAllFrames();
        emit updateTimeLine();
        break;
    case SETTING::FRAME_POOL_SIZE:
        mObject->setActiveFramePoolSize(mPreferenceManager->getInt(SETTING::FRAME_POOL_SIZE));
        break;
    default:
        break;
    }
}

void Editor::cut()
{
    copy();
    mScribbleArea->deleteSelection();
    mScribbleArea->deselectAll();
}

void Editor::copy()
{
    Layer* layer = mObject->getLayer(layers()->currentLayerIndex());
    if (layer == NULL) 
    {
        return;
    }

    if (layer->type() == Layer::BITMAP)
    {
        LayerBitmap* layerBitmap = (LayerBitmap*)layer;
        if (mScribbleArea->isSomethingSelected())
        {
            g_clipboardBitmapImage = layerBitmap->getLastBitmapImageAtFrame(currentFrame(), 0)->copy(mScribbleArea->getSelection().toRect());  // copy part of the image
        }
        else
        {
            g_clipboardBitmapImage = layerBitmap->getLastBitmapImageAtFrame(currentFrame(), 0)->copy();  // copy the whole image
        }
        clipboardBitmapOk = true;
        if (g_clipboardBitmapImage.image() != NULL)
            QApplication::clipboard()->setImage(*g_clipboardBitmapImage.image());
    }
    if (layer->type() == Layer::VECTOR)
    {
        clipboardVectorOk = true;
        g_clipboardVectorImage = *(((LayerVector*)layer)->getLastVectorImageAtFrame(currentFrame(), 0));  // copy the image
    }
}

void Editor::paste()
{
    Layer* layer = mObject->getLayer(layers()->currentLayerIndex());
    if (layer != NULL)
    {
        backups()->saveStates();
        if (layer->type() == Layer::BITMAP && g_clipboardBitmapImage.image() != NULL)
        {
            BitmapImage tobePasted = g_clipboardBitmapImage.copy();

            // TODO: paste doesn't remember location, will always paste on top of old image.
            qDebug() << "to be pasted --->" << tobePasted.image()->size();
            if (mScribbleArea->isSomethingSelected())
            {
                QRectF selection = mScribbleArea->getSelection();
                if (g_clipboardBitmapImage.width() <= selection.width() && g_clipboardBitmapImage.height() <= selection.height())
                {
                    tobePasted.moveTopLeft(selection.topLeft());
                }
                else
                {
                    tobePasted.transform(selection, true);
                }
            }
            auto pLayerBitmap = static_cast<LayerBitmap*>(layer);
            pLayerBitmap->getLastBitmapImageAtFrame(currentFrame(), 0)->paste(&tobePasted); // paste the clipboard

            backups()->bitmap("Bitmap: Paste");
        }
        else if (layer->type() == Layer::VECTOR && clipboardVectorOk)
        {
            mScribbleArea->deselectAll();
            VectorImage* vectorImage = ((LayerVector*)layer)->getLastVectorImageAtFrame(currentFrame(), 0);
            vectorImage->paste(g_clipboardVectorImage);  // paste the clipboard

            mScribbleArea->setSelection(vectorImage->getSelectionRect());
            backups()->vector("Vector: Paste");
        }
    }
    mScribbleArea->updateCurrentFrame();
}

void Editor::flipSelection(bool flipVertical)
{
    mScribbleArea->flipSelection(flipVertical);
}

void Editor::deselectAllSelections()
{
    backups()->deselect();
    emit deselectAll();
}

void Editor::deselectAllAndCancelTransform()
{
    backups()->deselect();
    emit deselectAll();
}

void Editor::clipboardChanged()
{
    if (clipboardBitmapOk == false)
    {
        g_clipboardBitmapImage.setImage(new QImage(QApplication::clipboard()->image()));
        g_clipboardBitmapImage.bounds() = QRect(g_clipboardBitmapImage.topLeft(), g_clipboardBitmapImage.image()->size());
        //qDebug() << "New clipboard image" << g_clipboardBitmapImage.image()->size();
    }
    else
    {
        clipboardBitmapOk = false;
        //qDebug() << "The image has been saved in the clipboard";
    }
}

int Editor::allLayers()
{
    return mScribbleArea->showAllLayers();
}

void Editor::toggleShowAllLayers()
{
    mScribbleArea->toggleShowAllLayers();
    emit updateTimeLine();
}

void Editor::toogleOnionSkinType()
{
    QString onionSkinState = mPreferenceManager->getString(SETTING::ONION_TYPE);
    QString newState;
    if (onionSkinState == "relative")
    {
        newState = "absolute";
    }
    else
    {
        newState = "relative";
    }

    mPreferenceManager->set(SETTING::ONION_TYPE, newState);
}

Status Editor::setObject(Object* newObject)
{
    if (newObject == nullptr)
    {
        Q_ASSERT(false);
        return Status::INVALID_ARGUMENT;
    }

    if (newObject == mObject.get())
    {
        return Status::SAFE;
    }

    mObject.reset(newObject);

    for (BaseManager* m : mAllManagers)
    {
        m->load(mObject.get());
    }

    g_clipboardVectorImage.setObject(newObject);

    updateObject();

    if (mViewManager)
    {
        connect(newObject, &Object::layerViewChanged, mViewManager, &ViewManager::viewChanged);
    }

    emit objectLoaded();

    return Status::OK;
}

void Editor::updateObject()
{
    scrubTo(mObject->data()->getCurrentFrame());
    setCurrentLayerIndex(mObject->data()->getCurrentLayer());

    mAutosaveCounter = 0;
    mAutosaveNerverAskAgain = false;

    if (mScribbleArea)
    {
        mScribbleArea->updateAllFrames();
    }
    
    if (mPreferenceManager)
    {
        mObject->setActiveFramePoolSize(mPreferenceManager->getInt(SETTING::FRAME_POOL_SIZE));
    }

    emit updateLayerCount();
}

/* TODO: Export absolutely does not belong here, but due to the messed up project structure
 * there isn't really any better place atm. Once we do have a proper structure in place, this
 * should go somewhere else */
bool Editor::exportSeqCLI(QString filePath, LayerCamera *cameraLayer, QString format, int width, int height, int startFrame, int endFrame, bool transparency, bool antialias)
{
    if (width < 0)
    {
        width = cameraLayer->getViewRect().width();
    }
    if (height < 0)
    {
        height = cameraLayer->getViewRect().height();
    }
    if (startFrame < 1)
    {
        startFrame = 1;
    }
    if (endFrame < -1)
    {
        endFrame = mLayerManager->animationLength();
    }
    if (endFrame < 0)
    {
        endFrame = mLayerManager->animationLength(false);
    }

    QSize exportSize = QSize(width, height);
    mObject->exportFrames(startFrame,
                          endFrame,
                          cameraLayer,
                          exportSize,
                          filePath,
                          format,
                          transparency,
                          antialias,
                          NULL,
                          0);
    return true;
}

bool Editor::exportMovieCLI(QString filePath, LayerCamera *cameraLayer, int width, int height, int startFrame, int endFrame)
{
    if (width < 0)
    {
        width = cameraLayer->getViewRect().width();
    }
    if (height < 0)
    {
        height = cameraLayer->getViewRect().height();
    }
    if (startFrame < 1)
    {
        startFrame = 1;
    }
    if (endFrame < -1)
    {
        endFrame = mLayerManager->animationLength();
    }
    if (endFrame < 0)
    {
        endFrame = mLayerManager->animationLength(false);
    }

    QSize exportSize = QSize(width, height);

    ExportMovieDesc desc;
    desc.strFileName = filePath;
    desc.startFrame = startFrame;
    desc.endFrame = endFrame;
    desc.fps = playback()->fps();
    desc.exportSize = exportSize;
    desc.strCameraName = cameraLayer->name();

    MovieExporter ex;
    ex.run(object(), desc, [](float,float){}, [](float){}, [](QString){});
    return true;
}

QString Editor::workingDir() const
{
    return mObject->workingDir();
}


bool Editor::importBitmapImage(QString filePath, int space)
{
    QImageReader reader(filePath);

    Q_ASSERT(layers()->currentLayer()->type() == Layer::BITMAP);
    auto layer = static_cast<LayerBitmap*>(layers()->currentLayer());

    QImage img(reader.size(), QImage::Format_ARGB32_Premultiplied);
    if (img.isNull())
    {
        return false;
    }

    backups()->saveStates();

    std::map<int, KeyFrame*, std::greater<int>> canvasKeyFrames;

    for(auto map : layer->getKeysInLayer())
    {
        // make sure file is loaded when trying to get bitmap from layer...
        map.second->loadFile();
        canvasKeyFrames.insert(std::make_pair(map.first, map.second->clone()));
    }

    std::map<int, KeyFrame*, std::less<int>> importedKeyFrames;
    while (reader.read(&img))
    {
        bool keyExisted = layer->keyExists(currentFrame());
        bool keyAdded = false;
        KeyFrame* newKey = nullptr;
        if (!keyExisted)
        {
            newKey = addNewKey();
            keyAdded = true;
        }

        BitmapImage* bitmapImage = layer->getBitmapImageAtFrame(currentFrame());
        BitmapImage importedBitmapImage(mScribbleArea->getCentralPoint().toPoint() - QPoint(img.width() / 2, img.height() / 2), img);
        bitmapImage->paste(&importedBitmapImage);

        newKey = bitmapImage;
        if (newKey != nullptr) {
            newKey = newKey->clone();
        }
        importedKeyFrames.insert(std::make_pair(newKey->pos(), newKey));

        if (space > 1)
        {
            scrubTo(currentFrame() + space);
        }
        else
        {
            scrubTo(currentFrame() + 1);
        }


        // Workaround for tiff import getting stuck in this loop
        if (!reader.supportsAnimation())
        {
            break;
        }
    }
    backups()->importBitmap(canvasKeyFrames, importedKeyFrames);

    return true;
}

bool Editor::importVectorImage(QString filePath, bool /*isSequence*/)
{
    Q_ASSERT(layers()->currentLayer()->type() == Layer::VECTOR);

    auto layer = static_cast<LayerVector*>(layers()->currentLayer());

    backups()->saveStates();
    VectorImage* vectorImage = ((LayerVector*)layer)->getVectorImageAtFrame(currentFrame());
    if (vectorImage == NULL)
    {
        addNewKey();
        vectorImage = ((LayerVector*)layer)->getVectorImageAtFrame(currentFrame());
    }

    VectorImage importedVectorImage;
    bool ok = importedVectorImage.read(filePath);
    if (ok)
    {
        importedVectorImage.selectAll();
        vectorImage->paste(importedVectorImage);

        backups()->vector("Vector: Import");
    }

    return ok;
}

bool Editor::importImage(QString filePath, bool isSequence)
{
    Layer* layer = layers()->currentLayer();

    switch (layer->type())
    {
    case Layer::BITMAP:
        return importBitmapImage(filePath, isSequence);

    case Layer::VECTOR:
        return importVectorImage(filePath, isSequence);

    default:
    {
        //mLastError = Status::ERROR_INVALID_LAYER_TYPE;
        return false;
    }
    }
}

bool Editor::importGIF(QString filePath, int numOfImages)
{
    Layer* layer = layers()->currentLayer();
    if (layer->type() == Layer::BITMAP) {
        return importBitmapImage(filePath, numOfImages);
    } else {
        return false;
    }
}

void Editor::updateFrame(int frameNumber)
{
    mScribbleArea->updateFrame(frameNumber);
}

void Editor::updateFrameAndVector(int frameNumber)
{
    mScribbleArea->updateAllVectorLayersAt(frameNumber);
}

void Editor::updateCurrentFrame()
{
    mScribbleArea->updateCurrentFrame();
}

void Editor::setCurrentLayerIndex(int i)
{
    mCurrentLayerIndex = i;

    Layer* layer = mObject->getLayer(i);
    for (auto mgr : mAllManagers)
    {
        mgr->workingLayerChanged(layer);
    }
}

void Editor::scrubTo(int frame)
{
    if (frame < 1) { frame = 1; }
    int oldFrame = mFrame;
    mFrame = frame;

    Q_EMIT currentFrameChanged(oldFrame);
    Q_EMIT currentFrameChanged(frame);

    // FIXME: should not emit Timeline update here.
    // Editor must be an individual class.
    // Will remove all Timeline related code in Editor class.
    if (mPlaybackManager && !mPlaybackManager->isPlaying())
    {
        emit updateTimeLine(); // needs to update the timeline to update onion skin positions
    }
    mObject->updateActiveFrames(frame);
}

void Editor::scrubForward()
{
    scrubTo(currentFrame() + 1);
}

void Editor::scrubBackward()
{
    if (currentFrame() > 1)
    {
        scrubTo(currentFrame() - 1);
    }
}

KeyFrame* Editor::addNewKey()
{
    return addKeyFrame(layers()->currentLayerIndex(), currentFrame());
}

KeyFrame* Editor::addKeyFrame(int layerIndex, int frameIndex)
{
    addKeyFrame(layerIndex, frameIndex, false);
}

/**
 * @brief Editor::addKeyFrame
 * @param layerIndex
 * @param frameIndex
 * @param isLastFrame Set true if no more frames will be added, otherwise false.
 * @return KeyFrame*
 */
KeyFrame* Editor::addKeyFrame(int layerIndex, int frameIndex, bool ignoreKeyExists)
{
    Layer* layer = mObject->getLayer(layerIndex);
    if (layer == NULL)
    {
        Q_ASSERT(false);
        return nullptr;
    }

    if (!ignoreKeyExists)
    {
        while (layer->keyExists(frameIndex))
        {
            frameIndex += 1;
        }
    }

    bool ok = layer->addNewKeyFrameAt(frameIndex);
    if (ok)
    {
        scrubTo(frameIndex); // currentFrameChanged() emit inside.
    }

    if (layerIndex != currentLayerIndex())
    {
        setCurrentLayerIndex(layerIndex);
    }

    return layer->getKeyFrameAt(frameIndex);
}

KeyFrame* Editor::addKeyFrameToLayerId(int layerId, int frameIndex)
{
    return addKeyFrameToLayerId(layerId,frameIndex, false);
}

KeyFrame* Editor::addKeyFrameToLayerId(int layerId, int frameIndex, bool ignoreKeyExists)
{
    Layer* layer = layers()->findLayerById(layerId);
    int layerIndex = layers()->getLayerIndex(layer);
    if (layer == NULL)
    {
        Q_ASSERT(false);
        return nullptr;
    }

    if (!ignoreKeyExists)
    {
        while (layer->keyExists(frameIndex) && frameIndex > 1)
        {
            frameIndex += 1;
        }
    }

    bool ok = layer->addNewKeyFrameAt(frameIndex);
    if (ok)
    {
        scrubTo(frameIndex); // currentFrameChanged() emit inside.
    }

    if (layerIndex != currentLayerIndex())
    {
        setCurrentLayerIndex(layerIndex);
    }

    return layer->getKeyFrameAt(frameIndex);
}


/**
 * @brief Editor::addKeyContaining
 * Add a keyframe to the timeline which contains a keyframe
 * @param layerIndex
 * @param frameIndex
 * @param key
 *
 */
void Editor::addKeyContaining(int layerId, int frameIndex, KeyFrame* key)
{
    Layer* layer = layers()->findLayerById(layerId);
    int layerIndex = layers()->getLayerIndex(layer);
    while (layer->keyExistsWhichCovers(frameIndex))
    {
        frameIndex += 1;
    }

    bool ok = layer->addKeyFrame(frameIndex, key);
    if (ok)
    {
        scrubTo(frameIndex); // currentFrameChanged() emit inside.
    }

    if (layerIndex != currentLayerIndex())
    {
        setCurrentLayerIndex(layerIndex);
    }

}

void Editor::removeKeyAt(int layerIndex, int frameIndex)
{
    Layer* layer = layers()->getLayer(layerIndex);
    if (!layer->keyExistsWhichCovers(frameIndex))
    {
        return;
    }

    layer->removeKeyFrame(frameIndex);

    while (!layer->keyExists(frameIndex) && frameIndex > 1)
    {
        frameIndex -= 1;
    }

    scrubPreviousKeyFrame();

    if (layerIndex != currentLayerIndex())
    {
        setCurrentLayerIndex(layerIndex);
    }

    Q_EMIT layers()->currentLayerChanged(layerIndex); // trigger timeline repaint.
}

void Editor::removeKeyAtLayerId(int layerId, int frameIndex)
{
    Layer* layer = layers()->findLayerById(layerId);
    int layerIndex = layers()->getLayerIndex(layer);
    if (!layer->keyExistsWhichCovers(frameIndex))
    {
        return;
    }

    layer->removeKeyFrame(frameIndex);

    while (!layer->keyExists(frameIndex) && frameIndex > 1)
    {
        frameIndex -= 1;
    }

    scrubTo(frameIndex);

    if (layerIndex != currentLayerIndex())
    {
        setCurrentLayerIndex(layerIndex);
    }

    Q_EMIT layers()->currentLayerChanged(layerIndex); // trigger timeline repaint.
}

void Editor::removeCurrentKey()
{
    removeKeyAt(currentLayerIndex(), currentFrame());
}

void Editor::scrubNextKeyFrame()
{
    Layer* layer = layers()->currentLayer();
    Q_ASSERT(layer);

    int nextPosition = layer->getNextKeyFramePosition(currentFrame());
    scrubTo(nextPosition);
}

void Editor::scrubPreviousKeyFrame()
{
    Layer* layer = mObject->getLayer(layers()->currentLayerIndex());
    Q_ASSERT(layer);

    int prevPosition = layer->getPreviousKeyFramePosition(currentFrame());
    scrubTo(prevPosition);
}

void Editor::switchVisibilityOfLayer(int layerNumber)
{
    Layer* layer = mObject->getLayer(layerNumber);
    if (layer != NULL) layer->switchVisibility();
    mScribbleArea->updateAllFrames();

    emit updateTimeLine();
}

void Editor::moveLayer(int i, int j)
{
    mObject->moveLayer(i, j);
    if (j < i)
    {
        layers()->setCurrentLayer(j);
    }
    else
    {
        layers()->setCurrentLayer(j);
    }
    emit updateTimeLine();
    mScribbleArea->updateAllFrames();
}

void Editor::prepareSave()
{
    for (auto mgr : mAllManagers)
    {
        mgr->save(mObject.get());
    }
}

void Editor::clearCurrentFrame()
{
    mScribbleArea->clearImage();
}
