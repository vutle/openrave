// Copyright (C) 2006-2008 Carnegie Mellon University (rdiankov@cs.cmu.edu)
//
// This file is part of OpenRAVE.
// OpenRAVE is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
#include "qtcoin.h"

#include <GL/glew.h> // Handles OpenGL extensions
#include <Inventor/elements/SoGLCacheContextElement.h>

#include <Inventor/actions/SoWriteAction.h>
#include <Inventor/nodes/SoComplexity.h>
#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoFaceSet.h>
#include <Inventor/nodes/SoPointSet.h>
#include <Inventor/nodes/SoLineSet.h>
#include <Inventor/nodes/SoMaterialBinding.h>
#include <Inventor/nodes/SoTransparencyType.h>
#include <Inventor/events/SoLocation2Event.h>
#include <Inventor/SoPickedPoint.h>

#if QT_VERSION >= 0x040000 // check for qt4
#include <QtOpenGL/QGLWidget>
#else
#include <qgl.h>
#endif

//const QMetaObject RaveViewer::staticMetaObject;
const float TIMER_SENSOR_INTERVAL = (1.0f/60.0f);

#define VIDEO_WIDTH 640
#define VIDEO_HEIGHT 480
#define VIDEO_FRAMERATE 30 //60

//class MySoQtExaminerViewer : public SoQtExaminerViewer
//{
//public:
//    MySoQtExaminerViewer(QWidget *parent=NULL) : SoQtExaminerViewer(parent) {}
//    
//protected:
//    
//    SbBool processSoEvent(const SoEvent *const event)
//    {
//        if( event->getTypeId() == SoLocation2Event::getClassTypeId() ) {
//            RAVELOG_INFOA("mouse: %d, %d\n",(int)event->getPosition()[0],(int)event->getPosition()[1]);
//        }
//            
//        return SoQtExaminerViewer::processSoEvent(event);
//    }
//};

int QtCoinViewer::s_InitRefCount = 0;

QtCoinViewer::QtCoinViewer(EnvironmentBase* penv)
#if QT_VERSION >= 0x040000 // check for qt4
    : QMainWindow(NULL, Qt::Window), RaveViewerBase(penv),
#else
      : QMainWindow(NULL, "OpenRAVE", Qt::WType_TopLevel), RaveViewerBase(penv),
#endif
_ivOffscreen(SbViewportRegion(VIDEO_WIDTH, VIDEO_HEIGHT))
{
#if QT_VERSION >= 0x040000 // check for qt4
    setWindowTitle("OpenRAVE");
    statusBar()->showMessage(tr("Status Bar"));
#endif
    _bAVIInit = false;
    pTogglePublishAnytime = pToggleDynamicSimulation = NULL;
    _pToggleDebug = NULL;

    pthread_mutex_init(&_mutexMessages, NULL);
    pthread_mutex_init(&_mutexUpdating, NULL);
    pthread_mutex_init(&_mutexMouseMove, NULL);

    //vlayout = new QVBoxLayout(this);
    view1 = new QGroupBox(this);
    //vlayout->addWidget(view1, 1);
    setCentralWidget (view1);

    resize(640, 480);

    _pviewer = new SoQtExaminerViewer(view1);

    _pdragger = NULL;
    _selectedNode = NULL;
    _pSelectedItem = NULL;

    // initialize the environment
    _ivRoot = new SoSelection();
    _ivRoot->ref();
    _ivRoot->policy.setValue(SoSelection::SHIFT);
    
    _ivCamera = new SoPerspectiveCamera();
    _ivStyle = new SoDrawStyle();
    _ivCamera->position.setValue(-0.5f, 1.5f, 0.8f);
    _ivCamera->orientation.setValue(SbVec3f(1,0,0), -0.5f);
    _ivCamera->aspectRatio = (float)view1->size().width() / (float)view1->size().height();

    _ivBodies = new SoSeparator();

    SoSeparator* pmsgsep = new SoSeparator();
    SoTranslation* pmsgtrans = new SoTranslation();
    pmsgtrans->translation.setValue(SbVec3f(-0.98,0.94,0));
    pmsgsep->addChild(pmsgtrans);
    SoBaseColor* pcolor = new SoBaseColor();
    pcolor->rgb.setValue(0,0,0);
    pmsgsep->addChild(pcolor);
    _messageNode = new SoText2();
    pmsgsep->addChild(_messageNode);

    _ivRoot->addChild(pmsgsep);
    _ivRoot->addChild(_ivCamera);

    SoEventCallback * ecb = new SoEventCallback;
    ecb->addEventCallback(SoLocation2Event::getClassTypeId(), mousemove_cb, this);
    _ivRoot->addChild(ecb);

    _ivRoot->addChild(_ivStyle);
    _ivRoot->addChild(_ivBodies);

    // add Inventor selection callbacks
    _ivRoot->addSelectionCallback(_SelectHandler, this);
    _ivRoot->addDeselectionCallback(_DeselectHandler, this);

    _pFigureRoot = new SoSeparator();
    SoComplexity* pcomplexity = new SoComplexity();
    pcomplexity->value = 0.1f; // default =0.5, lower is faster
    pcomplexity->type = SoComplexity::SCREEN_SPACE;
    _pFigureRoot->addChild(pcomplexity);
    _ivRoot->addChild(_pFigureRoot);
    
    _pviewer->setSceneGraph(_ivRoot);
    _pviewer->setAutoClippingStrategy(SoQtViewer::CONSTANT_NEAR_PLANE, 0.01f);
    _pviewer->setSeekTime(1.0f);

    _SetBkgndColor(Vector(1,1,1));

    // setup a callback handler for keyboard events
    _eventKeyboardCB = new SoEventCallback;
    _ivRoot->addChild(_eventKeyboardCB);
    _eventKeyboardCB->addEventCallback(SoKeyboardEvent::getClassTypeId(), _KeyHandler, this);

    _timerSensor = new SoTimerSensor(GlobAdvanceFrame, this);
    _timerSensor->setInterval(SbTime(TIMER_SENSOR_INTERVAL));

    _timerVideo = new SoTimerSensor(GlobVideoFrame, this);
    _timerVideo->setInterval(SbTime(1/(float)VIDEO_FRAMERATE));

    _renderAction = NULL;
    _altDown[0] = _altDown[1]  = false;
    _ctrlDown[0] = _ctrlDown[1] = false;
    _bAVIInit = false;
    _bSaveVideo = false;
    _bRealtimeVideo = false;
    _bUpdateEnvironment = true;
    
    // toggle switches
    _nFrameNum = 0;
    _bDisplayGrid = false;
    _bDisplayIK = false;
    _bDisplayFPS = false;
    _bJointHilit = true;
    _bDynamicReplan = false;
    _bVelPredict = true;
    _bDynSim = false;
    _bControl = true;
    _bGravity = true;
    _bTimeElapsed = false;
    _bSensing = false;
    _bMemory = true;
    _bHardwarePlan = false;
    _bShareBitmap = true;
    _bManipTracking = false;
    _bAntialiasing = false;
    _glInit = false;
    _viewGeometryMode = VG_RenderOnly;
    _bSelfCollision = false;

    SetupMenus();

    InitOffscreenRenderer();

    if (!_timerVideo->isScheduled())
        _timerVideo->schedule(); 

    pphysics = NULL;
}

QtCoinViewer::~QtCoinViewer()
{
    RAVELOG_DEBUGA("destroying qtcoinviewer\n");

    {
        MutexLock m(&_mutexMessages);

        list<EnvMessage*>::iterator itmsg;
        FORIT(itmsg, _listMessages)
            (*itmsg)->viewerexecute(); // have to execute instead of deleteing since there can be threads waiting

        list<pthread_mutex_t*>::iterator it = listMsgMutexes.begin();
        FORIT(it, listMsgMutexes) {
            pthread_mutex_destroy(*it);
            delete *it;
        }
        
        _listMessages.clear();
        listMsgMutexes.clear();
    }

    if( _bAVIInit ) {
        RAVEPRINT(L"stopping avi\n");
        STOP_AVI();
    }

    if(_glInit)
      FinishGL();

    _ivRoot->unref();
    _pOffscreenVideo->unref();
    _ivRoot->deselectAll();

    if (_timerSensor->isScheduled())
        _timerSensor->unschedule(); 
    if (_timerVideo->isScheduled())
        _timerVideo->unschedule(); 

    _eventKeyboardCB->removeEventCallback(SoKeyboardEvent::getClassTypeId(), _KeyHandler, this);
    _ivRoot->removeSelectionCallback(_SelectHandler, this);
    _ivRoot->removeDeselectionCallback(_DeselectHandler, this);
    _eventKeyboardCB->unref();

    pthread_mutex_destroy(&_mutexMessages);
    pthread_mutex_destroy(&_mutexUpdating);
    pthread_mutex_destroy(&_mutexMouseMove);

    // don't dereference
//    if( --s_InitRefCount <= 0 )
//        SoQt::done();
}

void QtCoinViewer::resize ( int w, int h)
{
    QMainWindow::resize(w,h);
}

void QtCoinViewer::resize ( const QSize & qs)
{
    resize(qs.width(), qs.height());
}

void QtCoinViewer::mousemove_cb(void * userdata, SoEventCallback * node)
{
    ((QtCoinViewer*)userdata)->_mousemove_cb(node);
}

void QtCoinViewer::_mousemove_cb(SoEventCallback * node)
{
    SoRayPickAction rp( _pviewer->getViewportRegion() );
    rp.setPoint(node->getEvent()->getPosition());
    rp.apply(_ivRoot);
    SoPickedPoint * pt = rp.getPickedPoint(0);
    if( pt != NULL ) {
        SoPath* path = pt->getPath();
        Item *pItem = NULL;
        SoNode* node = NULL;
        for(int i = path->getLength()-1; i >= 0; --i) {
            node = path->getNode(i);

            // search the environment
            FOREACH(it, _mapbodies) {
                assert( it->second != NULL );
                if (it->second->ContainsIvNode(node)) {
                    pItem = it->second;
                    break;
                }
            }

            if( pItem != NULL )
                break;
        }
        
        if (pItem != NULL) {
            KinBodyItem* pKinBody = dynamic_cast<KinBodyItem*>(pItem);
            KinBody::Link* pSelectedLink = pKinBody != NULL ? pKinBody->GetLinkFromIv(node) : NULL;

            stringstream ss;
            ss << "mouse on " << _stdwcstombs(pKinBody->GetBody()->GetName()) << ":";
            if( pSelectedLink != NULL )
                ss << _stdwcstombs(pSelectedLink->GetName());
            else
                ss << "(NULL)";
            ss << " (" << std::fixed << std::setprecision(4)
               << std::setw(8) << std::left << pt->getPoint()[0] << ", "
               << std::setw(8) << std::left << pt->getPoint()[1] << ", "
               << std::setw(8) << std::left << pt->getPoint()[2] << ")" << endl;

            MutexLock m(&_mutexMessages);
            _strMouseMove = ss.str();
        }
        else {
            MutexLock m(&_mutexMessages);
            _strMouseMove.resize(0);
        }
    }
    else {
        MutexLock m(&_mutexMessages);
        _strMouseMove.resize(0);
    }
}

class ViewerSetSizeMessage : public QtCoinViewer::EnvMessage
{
public:
    ViewerSetSizeMessage(QtCoinViewer* pviewer, void** ppreturn, int width, int height)
        : EnvMessage(pviewer, ppreturn, false), _width(width), _height(height) {}
    
    virtual void viewerexecute() {
        _pviewer->_ViewerSetSize(_width, _height);
        EnvMessage::viewerexecute();
    }

private:
    int _width, _height;
};

void QtCoinViewer::ViewerSetSize(int w, int h)
{
    EnvMessage* pmsg = new ViewerSetSizeMessage(this, NULL, w, h);
    pmsg->callerexecute();
}

void QtCoinViewer::_ViewerSetSize(int w, int h)
{
    resize(w,h);
}

class ViewerMoveMessage : public QtCoinViewer::EnvMessage
{
public:
    ViewerMoveMessage(QtCoinViewer* pviewer, void** ppreturn, int x, int y)
        : EnvMessage(pviewer, ppreturn, false), _x(x), _y(y) {}
    
    virtual void viewerexecute() {
        _pviewer->_ViewerMove(_x, _y);
        EnvMessage::viewerexecute();
    }

private:
    int _x, _y;
};

void QtCoinViewer::ViewerMove(int x, int y)
{
    EnvMessage* pmsg = new ViewerMoveMessage(this, NULL, x, y);
    pmsg->callerexecute();
}

void QtCoinViewer::_ViewerMove(int x, int y)
{
    move(x,y);
}

class ViewerSetTitleMessage : public QtCoinViewer::EnvMessage
{
public:
    ViewerSetTitleMessage(QtCoinViewer* pviewer, void** ppreturn, const char* ptitle)
        : EnvMessage(pviewer, ppreturn, false), _title(ptitle) {}
    
    virtual void viewerexecute() {
        _pviewer->_ViewerSetTitle(_title.c_str());
        EnvMessage::viewerexecute();
    }

private:
    string _title;
};

void QtCoinViewer::ViewerSetTitle(const char* ptitle)
{
    if( ptitle == NULL )
        return;

    EnvMessage* pmsg = new ViewerSetTitleMessage(this, NULL, ptitle);
    pmsg->callerexecute();
}

void QtCoinViewer::_ViewerSetTitle(const char* ptitle)
{
    setWindowTitle(ptitle);
}

bool QtCoinViewer::LoadModel(const char* pfilename)
{
    if( pfilename == NULL )
        return false;

    SoInput mySceneInput;
    if (mySceneInput.openFile(pfilename)) {
        GetRoot()->addChild(SoDB::readAll(&mySceneInput));
        return true;
    }

    return false;
}

void QtCoinViewer::_StartPlaybackTimer()
{
    if (!_timerSensor->isScheduled())
        _timerSensor->schedule(); 
}

void QtCoinViewer::_StopPlaybackTimer()
{
    if (_timerSensor->isScheduled())
        _timerSensor->unschedule(); 
}

class GetFractionOccludedMessage : public QtCoinViewer::EnvMessage
{
public:
    GetFractionOccludedMessage(QtCoinViewer* pviewer, void** ppreturn, KinBody* pbody, int width, int height,
                               float nearPlane, float farPlane, const RaveTransform<float>& extrinsic,
                               const float* pKK, double& fracOccluded)
            : EnvMessage(pviewer, ppreturn, true), _pbody(pbody), _width(width), _height(height), _nearPlane(nearPlane),
              _farPlane(farPlane), _extrinsic(extrinsic), _pKK(pKK), _fracOccluded(fracOccluded) {}
    
    virtual void viewerexecute() {
        void* ret = (void*)_pviewer->_GetFractionOccluded(_pbody, _width, _height, _nearPlane, _farPlane, _extrinsic, _pKK, _fracOccluded);
        if( _ppreturn != NULL )
            *_ppreturn = ret;
        EnvMessage::viewerexecute();
    }

private:
    KinBody* _pbody;
    int _width, _height;
    float _nearPlane, _farPlane;
    const RaveTransform<float>& _extrinsic;
    const float* _pKK;
    double& _fracOccluded;
};

bool QtCoinViewer::GetFractionOccluded(KinBody* pbody, int width, int height, float nearPlane, float farPlane, const RaveTransform<float>& extrinsic, const float* pKK, double& fracOccluded)
{
    void* ret;
    if (_timerSensor->isScheduled()) {
        EnvMessage* pmsg = new GetFractionOccludedMessage(this, &ret, pbody, width, height, nearPlane, farPlane, extrinsic, pKK, fracOccluded);
        pmsg->callerexecute();
    }
    
    return *(bool*)&ret;
}

class GetCameraImageMessage : public QtCoinViewer::EnvMessage
{
public:
    GetCameraImageMessage(QtCoinViewer* pviewer, void** ppreturn,
                            void* pMemory, int width, int height, const RaveTransform<float>& extrinsic, const float* pKK)
            : EnvMessage(pviewer, ppreturn, true), _pMemory(pMemory), _width(width), _height(height), _extrinsic(extrinsic), _pKK(pKK) {
    }
    
    virtual void viewerexecute() {
        void* ret = (void*)_pviewer->_GetCameraImage(_pMemory, _width, _height, _extrinsic, _pKK);
        if( _ppreturn != NULL )
            *_ppreturn = ret;
        EnvMessage::viewerexecute();
    }

private:
    void* _pMemory;
    int _width, _height;
    const RaveTransform<float>& _extrinsic;
    const float* _pKK;
};

bool QtCoinViewer::GetCameraImage(void* pMemory, int width, int height, const RaveTransform<float>& extrinsic, const float* pKK)
{
    void* ret = NULL;
    if (_timerSensor->isScheduled()) {
        EnvMessage* pmsg = new GetCameraImageMessage(this, &ret, pMemory, width, height, extrinsic, pKK);
        pmsg->callerexecute();
    }
    
    return *(bool*)&ret;
}

class WriteCameraImageMessage : public QtCoinViewer::EnvMessage
{
public:
    WriteCameraImageMessage(QtCoinViewer* pviewer, void** ppreturn, int width, int height,
                            const RaveTransform<float>& t, const float* pKK, const char* fileName, const char* extension)
            : EnvMessage(pviewer, ppreturn, true), _width(width), _height(height), _t(t),
              _pKK(pKK), _fileName(fileName), _extension(extension) {}
    
    virtual void viewerexecute() {
        void* ret = (void*)_pviewer->_WriteCameraImage(_width, _height, _t, _pKK, _fileName, _extension);
        if( _ppreturn != NULL )
            *_ppreturn = ret;
        EnvMessage::viewerexecute();
    }

private:
    int _width, _height;
    const RaveTransform<float>& _t;
    const float *_pKK;
    const char* _fileName, *_extension;
};

bool QtCoinViewer::WriteCameraImage(int width, int height, const RaveTransform<float>& t, const float* pKK, const char* fileName, const char* extension)
{
    void* ret;
    if (_timerSensor->isScheduled()) {
        EnvMessage* pmsg = new WriteCameraImageMessage(this, &ret, width, height, t, pKK, fileName, extension);
        pmsg->callerexecute();
    }
    
    return *(bool*)&ret;
}

class SetCameraMessage : public QtCoinViewer::EnvMessage
{
public:
    SetCameraMessage(QtCoinViewer* pviewer, void** ppreturn,
                     const RaveVector<float>& pos, const RaveVector<float>& quat)
            : EnvMessage(pviewer, ppreturn, false), _pos(pos), _quat(quat) {}
    
    virtual void viewerexecute() {
        _pviewer->_SetCamera(_pos, _quat);
        EnvMessage::viewerexecute();
    }

private:
    const RaveVector<float> _pos, _quat;
};

void QtCoinViewer::SetCamera(const RaveVector<float>& pos, const RaveVector<float>& quat)
{
    EnvMessage* pmsg = new SetCameraMessage(this, NULL, pos, quat);
    pmsg->callerexecute();
}

class SetCameraLookAtMessage : public QtCoinViewer::EnvMessage
{
public:
    SetCameraLookAtMessage(QtCoinViewer* pviewer, void** ppreturn, const RaveVector<float>& lookat,
                           const RaveVector<float>& campos, const RaveVector<float>& camup)
        : EnvMessage(pviewer, ppreturn, false), _lookat(lookat), _campos(campos), _camup(camup) {}
    
    virtual void viewerexecute() {
        _pviewer->_SetCameraLookAt(_lookat, _campos, _camup);
        EnvMessage::viewerexecute();
    }

private:
    const RaveVector<float> _lookat, _campos, _camup;
};

void QtCoinViewer::SetCameraLookAt(const RaveVector<float>& lookat, const RaveVector<float>& campos, const RaveVector<float>& camup)
{
    EnvMessage* pmsg = new SetCameraLookAtMessage(this, NULL, lookat, campos, camup);
    pmsg->callerexecute();
}

class DrawMessage : public QtCoinViewer::EnvMessage
{
public:
    enum DrawType
    {
        DT_Point = 0,
        DT_Sphere,
        DT_LineStrip,
        DT_LineList,
    };

    DrawMessage(QtCoinViewer* pviewer, SoSeparator* pparent, const float* ppoints, int numPoints,
                int stride, float fwidth, const float* colors, DrawType type)
        : EnvMessage(pviewer, NULL, false), _numPoints(numPoints),
          _fwidth(fwidth), _pparent(pparent), _type(type)
    {
        _vpoints.resize(3*numPoints);
        for(int i = 0; i < numPoints; ++i) {
            _vpoints[3*i+0] = ppoints[0];
            _vpoints[3*i+1] = ppoints[1];
            _vpoints[3*i+2] = ppoints[2];
            ppoints = (float*)((char*)ppoints + stride);
        }
        _stride = 3*sizeof(float);

        _vcolors.resize(3*numPoints);
        if( colors != NULL )
            memcpy(&_vcolors[0], colors, numPoints*3*sizeof(float));

        _bManyColors = true;
    }
    DrawMessage(QtCoinViewer* pviewer, SoSeparator* pparent, const float* ppoints, int numPoints,
                        int stride, float fwidth, const RaveVector<float>& color, DrawType type)
        : EnvMessage(pviewer, NULL, false), _numPoints(numPoints),
          _fwidth(fwidth), _color(color), _pparent(pparent), _type(type)
    {
        _vpoints.resize(3*numPoints);
        for(int i = 0; i < numPoints; ++i) {
            _vpoints[3*i+0] = ppoints[0];
            _vpoints[3*i+1] = ppoints[1];
            _vpoints[3*i+2] = ppoints[2];
            ppoints = (float*)((char*)ppoints + stride);
        }
        _stride = 3*sizeof(float);

        _bManyColors = false;
    }
    
    virtual void viewerexecute() {
        void* ret=NULL;
        switch(_type) {
        case DT_Point:
            if( _bManyColors )
                ret = _pviewer->_plot3(_pparent, &_vpoints[0], _numPoints, _stride, _fwidth, &_vcolors[0]);
            else
                ret = _pviewer->_plot3(_pparent, &_vpoints[0], _numPoints, _stride, _fwidth, _color);

            break;
        case DT_Sphere:
            if( _bManyColors )
                ret = _pviewer->_drawspheres(_pparent, &_vpoints[0], _numPoints, _stride, _fwidth, &_vcolors[0]);
            else
                ret = _pviewer->_drawspheres(_pparent, &_vpoints[0], _numPoints, _stride, _fwidth, _color);

            break;
        case DT_LineStrip:
            if( _bManyColors )
                ret = _pviewer->_drawlinestrip(_pparent, &_vpoints[0], _numPoints, _stride, _fwidth, &_vcolors[0]);
            else
                ret = _pviewer->_drawlinestrip(_pparent, &_vpoints[0], _numPoints, _stride, _fwidth, _color);
            break;
        case DT_LineList:
            if( _bManyColors )
                ret = _pviewer->_drawlinelist(_pparent, &_vpoints[0], _numPoints, _stride, _fwidth, &_vcolors[0]);
            else
                ret = _pviewer->_drawlinelist(_pparent, &_vpoints[0], _numPoints, _stride, _fwidth, _color);
            break;
        }
        
        assert( _pparent == ret);
        EnvMessage::viewerexecute();
    }
    
private:
    vector<float> _vpoints;
    int _numPoints, _stride;
    float _fwidth;
    const RaveVector<float> _color;
    vector<float> _vcolors;
    SoSeparator* _pparent;

    bool _bManyColors;
    DrawType _type;
};

void* QtCoinViewer::plot3(const float* ppoints, int numPoints, int stride, float fPointSize, const RaveVector<float>& color, int drawstyle)
{
    void* pret = new SoSeparator();
    EnvMessage* pmsg = new DrawMessage(this, (SoSeparator*)pret, ppoints, numPoints, stride, fPointSize, color, drawstyle ? DrawMessage::DT_Sphere : DrawMessage::DT_Point);
    pmsg->callerexecute();

    return pret;
}

void* QtCoinViewer::plot3(const float* ppoints, int numPoints, int stride, float fPointSize, const float* colors, int drawstyle)
{
    void* pret = new SoSeparator();
    EnvMessage* pmsg = new DrawMessage(this, (SoSeparator*)pret, ppoints, numPoints, stride, fPointSize, colors, drawstyle ? DrawMessage::DT_Sphere : DrawMessage::DT_Point);
    pmsg->callerexecute();

    return pret;
}

void* QtCoinViewer::drawlinestrip(const float* ppoints, int numPoints, int stride, float fwidth, const RaveVector<float>& color)
{
    void* pret = new SoSeparator();
    EnvMessage* pmsg = new DrawMessage(this, (SoSeparator*)pret, ppoints, numPoints, stride, fwidth, color,DrawMessage::DT_LineStrip);
    pmsg->callerexecute();

    return pret;
}

void* QtCoinViewer::drawlinestrip(const float* ppoints, int numPoints, int stride, float fwidth, const float* colors)
{
    void* pret = new SoSeparator();
    EnvMessage* pmsg = new DrawMessage(this, (SoSeparator*)pret, ppoints, numPoints, stride, fwidth, colors, DrawMessage::DT_LineStrip);
    pmsg->callerexecute();

    return pret;
}

void* QtCoinViewer::drawlinelist(const float* ppoints, int numPoints, int stride, float fwidth, const RaveVector<float>& color)
{
    void* pret = new SoSeparator();
    EnvMessage* pmsg = new DrawMessage(this, (SoSeparator*)pret, ppoints, numPoints, stride, fwidth, color, DrawMessage::DT_LineList);
    pmsg->callerexecute();

    return pret;
}

void* QtCoinViewer::drawlinelist(const float* ppoints, int numPoints, int stride, float fwidth, const float* colors)
{
    void* pret = new SoSeparator();
    EnvMessage* pmsg = new DrawMessage(this, (SoSeparator*)pret, ppoints, numPoints, stride, fwidth, colors, DrawMessage::DT_LineList);
    pmsg->callerexecute();

    return pret;
}

class DrawArrowMessage : public QtCoinViewer::EnvMessage
{
public:
    DrawArrowMessage(QtCoinViewer* pviewer, SoSeparator* pparent, const RaveVector<float>& p1,
                     const RaveVector<float>& p2, float fwidth, const RaveVector<float>& color)
        : EnvMessage(pviewer, NULL, false), _p1(p1), _p2(p2), _color(color), _pparent(pparent), _fwidth(fwidth) {}
    
    virtual void viewerexecute() {
        void* ret = _pviewer->_drawarrow(_pparent, _p1, _p2, _fwidth, _color);
        assert( _pparent == ret );
        EnvMessage::viewerexecute();
    }

private:
    RaveVector<float> _p1, _p2, _color;
    SoSeparator* _pparent;
    float _fwidth;
};

void* QtCoinViewer::drawarrow(const RaveVector<float>& p1, const RaveVector<float>& p2, float fwidth, const RaveVector<float>& color)
{
    void* pret = new SoSeparator();
    EnvMessage* pmsg = new DrawArrowMessage(this, (SoSeparator*)pret, p1, p2, fwidth, color);
    pmsg->callerexecute();

    return pret;
}

class DrawBoxMessage : public QtCoinViewer::EnvMessage
{
public:
    DrawBoxMessage(QtCoinViewer* pviewer, SoSeparator* pparent,
                   const RaveVector<float>& vpos, const RaveVector<float>& vextents)
        : EnvMessage(pviewer, NULL, false), _vpos(vpos), _vextents(vextents), _pparent(pparent) {}
    
    virtual void viewerexecute() {
        void* ret = _pviewer->_drawbox(_pparent, _vpos, _vextents);
        assert( _ppreturn == ret );
        EnvMessage::viewerexecute();
    }
    
private:
    RaveVector<float> _vpos, _vextents;
    SoSeparator* _pparent;
};

void* QtCoinViewer::drawbox(const RaveVector<float>& vpos, const RaveVector<float>& vextents)
{
    void* pret = new SoSeparator();
    EnvMessage* pmsg = new DrawBoxMessage(this, (SoSeparator*)pret, vpos, vextents);
    pmsg->callerexecute();

    return pret;
}

class DrawTriMeshMessage : public QtCoinViewer::EnvMessage
{
public:
    DrawTriMeshMessage(QtCoinViewer* pviewer, SoSeparator* pparent, const float* ppoints, int stride, const int* pIndices, int numTriangles, const RaveVector<float>& color)
        : EnvMessage(pviewer, NULL, false), _color(color), _pparent(pparent)
    {
        _vpoints.resize(3*3*numTriangles);
        if( pIndices == NULL ) {
            for(int i = 0; i < 3*numTriangles; ++i) {
                _vpoints[3*i+0] = ppoints[0];
                _vpoints[3*i+1] = ppoints[1];
                _vpoints[3*i+2] = ppoints[2];
                ppoints = (float*)((char*)ppoints + stride);
            }
        }
        else {
            for(int i = 0; i < numTriangles*3; ++i) {
                float* p = (float*)((char*)ppoints +  stride * pIndices[i]);
                _vpoints[3*i+0] = p[0];
                _vpoints[3*i+1] = p[1];
                _vpoints[3*i+2] = p[2];
            }
        }

        _bManyColors = false;
    }
    
    virtual void viewerexecute() {
        void* ret;
        //if( _bManyColors )
        ret = _pviewer->_drawtrimesh(_pparent, &_vpoints[0], 3*sizeof(float), NULL, _vpoints.size()/9,_color);
        //else
        //ret = _pviewer->_drawtrimesh(_pparent, &_vpoints[0], _numPoints, _stride, _fwidth, _color, _drawstyle);
        
        assert( _pparent == ret);
        EnvMessage::viewerexecute();
    }
    
private:
    vector<float> _vpoints;
    const RaveVector<float> _color;
    vector<float> _vcolors;
    SoSeparator* _pparent;

    bool _bManyColors;
};

void* QtCoinViewer::drawtrimesh(const float* ppoints, int stride, const int* pIndices, int numTriangles, const RaveVector<float>& color)
{
    void* pret = new SoSeparator();
    EnvMessage* pmsg = new DrawTriMeshMessage(this, (SoSeparator*)pret, ppoints, stride, pIndices, numTriangles, color);
    pmsg->callerexecute();

    return pret;
}

class CloseGraphMessage : public QtCoinViewer::EnvMessage
{
public:
    CloseGraphMessage(QtCoinViewer* pviewer, void** ppreturn, void* handle)
        : EnvMessage(pviewer, ppreturn, false), _handle(handle) {}
    
    virtual void viewerexecute() {
        _pviewer->_closegraph(_handle);
        EnvMessage::viewerexecute();
    }
    
private:
    void* _handle;
};

void QtCoinViewer::closegraph(void* handle)
{
    EnvMessage* pmsg = new CloseGraphMessage(this, NULL, handle);
    pmsg->callerexecute();
}

class DeselectMessage : public QtCoinViewer::EnvMessage
{
public:
    DeselectMessage(QtCoinViewer* pviewer, void** ppreturn)
        : EnvMessage(pviewer, ppreturn, false) {}
    
    virtual void viewerexecute() {
        _pviewer->_deselect();
        EnvMessage::viewerexecute();
    }
};

void QtCoinViewer::deselect()
{
    EnvMessage* pmsg = new DeselectMessage(this, NULL);
    pmsg->callerexecute();
}

class ResetMessage : public QtCoinViewer::EnvMessage
{
public:
    ResetMessage(QtCoinViewer* pviewer, void** ppreturn)
        : EnvMessage(pviewer, ppreturn, true) {}
    
    virtual void viewerexecute() {
        _pviewer->_Reset();
        EnvMessage::viewerexecute();
    }
};

void QtCoinViewer::Reset()
{
    if (_timerSensor->isScheduled()) {
        EnvMessage* pmsg = new ResetMessage(this, NULL);
        pmsg->callerexecute();
    }
}
    
class SetBkgndColorMessage : public QtCoinViewer::EnvMessage
{
public:
    SetBkgndColorMessage(QtCoinViewer* pviewer, void** ppreturn, const RaveVector<float>& color)
        : EnvMessage(pviewer, ppreturn, false), _color(color) {}
    
    virtual void viewerexecute() {
        _pviewer->_SetBkgndColor(_color);
        EnvMessage::viewerexecute();
    }
    
private:
    RaveVector<float> _color;
};

void QtCoinViewer::SetBkgndColor(const RaveVector<float>& color)
{
    if (_timerSensor->isScheduled()) {
        EnvMessage* pmsg = new SetBkgndColorMessage(this, NULL, color);
        pmsg->callerexecute();
    }
}

void QtCoinViewer::StartPlaybackTimer()
{
    MutexLock m(&_mutexUpdating);
    _bUpdateEnvironment = true;
}

void QtCoinViewer::StopPlaybackTimer()
{
    MutexLock m(&_mutexUpdating);
    _bUpdateEnvironment = false;
}

void QtCoinViewer::_SetCamera(const RaveVector<float>& pos, const RaveVector<float>& quat)
{
    GetCamera()->position.setValue(pos.x, pos.y, pos.z);
    GetCamera()->orientation.setValue(quat.y, quat.z, quat.w, quat.x);
}

void QtCoinViewer::_SetCameraLookAt(const RaveVector<float>& lookat, const RaveVector<float>& campos, const RaveVector<float>& camup)
{
    RaveVector<float> dir = -(lookat - campos);
    float len = RaveSqrt(dir.lengthsqr3());
    GetCamera()->focalDistance = len;

    if( len > 1e-6 )
        dir *= 1/len;
    else
        dir = RaveVector<float>(0,0,1);

    RaveVector<float> up = camup - dir * dot3(dir,camup);
    len = up.lengthsqr3();
    if( len < 1e-8 ) {
        up = RaveVector<float>(0,1,0);
        up -= dir * dot3(dir,up);
        len = up.lengthsqr3();
        if( len < 1e-8 ) {
            up = RaveVector<float>(1,0,0);
            up -= dir * dot3(dir,up);
            len = up.lengthsqr3();
        }
    }

    up *= 1/RaveSqrt(len);
    
    RaveVector<float> right; right.Cross(up,dir);
    RaveTransformMatrix<float> t;
    t.m[0] = right.x; t.m[1] = up.x; t.m[2] = dir.x;
    t.m[4] = right.y; t.m[5] = up.y; t.m[6] = dir.y;
    t.m[8] = right.z; t.m[9] = up.z; t.m[10] = dir.z;
    ofstream of("temp.txt");
    of << lookat << endl << campos << endl << camup << endl << t << endl;

    _SetCamera(campos, RaveTransform<float>(t).rot);
}

void QtCoinViewer::_SetBkgndColor(const RaveVector<float>& color)
{
    _pviewer->setBackgroundColor(SbColor(color.x, color.y, color.z));
    _ivOffscreen.setBackgroundColor(SbColor(color.x, color.y, color.z));
}

void QtCoinViewer::PrintCamera()
{
    SbVec3f pos = GetCamera()->position.getValue();
    SbVec3f axis;
    float fangle;
    GetCamera()->orientation.getValue(axis, fangle);

    RAVEPRINT(L"Camera Transformation:\n"
              L"position: %f %f %f\n"
              L"orientation: axis=(%f %f %f), angle = %f (%f deg)\n"
              L"height angle: %f, focal dist: %f\n", pos[0], pos[1], pos[2],
              axis[0], axis[1], axis[2], fangle, fangle*180.0f/PI, GetCamera()->heightAngle.getValue(),
              GetCamera()->focalDistance.getValue());
}

RaveTransform<float> QtCoinViewer::GetCameraTransform()
{
    MutexLock m(&_mutexMessages);
    RaveTransform<float> t = Tcam;
    return t;
}

void* QtCoinViewer::_plot3(SoSeparator* pparent, const float* ppoints, int numPoints, int stride, float fPointSize, const RaveVector<float>& color)
{
    if( pparent == NULL )
        return NULL;
   
    SoMaterial* mtrl = new SoMaterial;
    mtrl->diffuseColor = SbColor(color.x, color.y, color.z);
    mtrl->ambientColor = SbColor(color.x, color.y, color.z);
    mtrl->setOverride(true);
    pparent->addChild(mtrl);
    
    SoCoordinate3* vprop = new SoCoordinate3();
    
    if( stride != sizeof(float)*3 ) {
        vector<float> mypoints(numPoints*3);
        for(int i = 0; i < numPoints; ++i) {
            mypoints[3*i+0] = ppoints[0];
            mypoints[3*i+1] = ppoints[1];
            mypoints[3*i+2] = ppoints[2];
            ppoints = (float*)((char*)ppoints + stride);
        }
        
        vprop->point.setValues(0,numPoints,(float(*)[3])&mypoints[0]);
    }
    else
        vprop->point.setValues(0,numPoints,(float(*)[3])ppoints);
    
    pparent->addChild(vprop);
    
    SoDrawStyle* style = new SoDrawStyle();
    style->style = SoDrawStyle::POINTS;
    style->pointSize = fPointSize;
    pparent->addChild(style);
    
    SoPointSet* pointset = new SoPointSet();
    pointset->numPoints.setValue(-1);
    
    pparent->addChild(pointset);
    
    _pFigureRoot->addChild(pparent);
    return pparent;
}

void* QtCoinViewer::_plot3(SoSeparator* pparent, const float* ppoints, int numPoints, int stride, float fPointSize, const float* colors)
{
    if( pparent == NULL )
        return NULL;

    SoMaterial* mtrl = new SoMaterial;
    mtrl->diffuseColor.setValues(0, numPoints, (float(*)[3])colors);
    mtrl->setOverride(true);
    pparent->addChild(mtrl);
    
    SoMaterialBinding* pbinding = new SoMaterialBinding();
    pbinding->value = SoMaterialBinding::PER_VERTEX;
    pparent->addChild(pbinding);
    
    SoCoordinate3* vprop = new SoCoordinate3();
    
    if( stride != sizeof(float)*3 ) {
        vector<float> mypoints(numPoints*3);
        for(int i = 0; i < numPoints; ++i) {
            mypoints[3*i+0] = ppoints[0];
            mypoints[3*i+1] = ppoints[1];
            mypoints[3*i+2] = ppoints[2];
            ppoints = (float*)((char*)ppoints + stride);
        }
        
        vprop->point.setValues(0,numPoints,(float(*)[3])&mypoints[0]);
    }
    else
        vprop->point.setValues(0,numPoints,(float(*)[3])ppoints);
    
    pparent->addChild(vprop);
    
    SoDrawStyle* style = new SoDrawStyle();
    style->style = SoDrawStyle::POINTS;
    style->pointSize = fPointSize;
    pparent->addChild(style);
    
    SoPointSet* pointset = new SoPointSet();
    pointset->numPoints.setValue(-1);
    
    pparent->addChild(pointset);

    _pFigureRoot->addChild(pparent);
    return pparent;
}

void* QtCoinViewer::_drawspheres(SoSeparator* pparent, const float* ppoints, int numPoints, int stride, float fPointSize, const RaveVector<float>& color)
{
    if( pparent == NULL || ppoints == NULL || numPoints <= 0 )
        return pparent;
   
    for(int i = 0; i < numPoints; ++i) {
        SoSeparator* psep = new SoSeparator();
        SoTransform* ptrans = new SoTransform();
        
        ptrans->translation.setValue(ppoints[0], ppoints[1], ppoints[2]);
        
        psep->addChild(ptrans);
        pparent->addChild(psep);
        
        // set a diffuse color
        SoMaterial* mtrl = new SoMaterial;
        mtrl->diffuseColor = SbColor(color.x, color.y, color.z);
        mtrl->ambientColor = SbColor(color.x, color.y, color.z);
        mtrl->setOverride(true);
        psep->addChild(mtrl);
        
        SoSphere* c = new SoSphere();
        c->radius = fPointSize;
        psep->addChild(c);
        
        ppoints = (float*)((char*)ppoints + stride);
    }

    _pFigureRoot->addChild(pparent);
    return pparent;
}

void* QtCoinViewer::_drawspheres(SoSeparator* pparent, const float* ppoints, int numPoints, int stride, float fPointSize, const float* colors)
{
    if( pparent == NULL || ppoints == NULL || numPoints <= 0 )
        return pparent;
    
    for(int i = 0; i < numPoints; ++i) {
        SoSeparator* psep = new SoSeparator();
        SoTransform* ptrans = new SoTransform();
        
        ptrans->translation.setValue(ppoints[0], ppoints[1], ppoints[2]);
        
        psep->addChild(ptrans);
        pparent->addChild(psep);
        
        // set a diffuse color
        SoMaterial* mtrl = new SoMaterial;
        mtrl->diffuseColor = SbColor(colors[3*i +0], colors[3*i +1], colors[3*i +2]);
        mtrl->ambientColor = SbColor(colors[3*i +0], colors[3*i +1], colors[3*i +2]);
        mtrl->setOverride(true);
        psep->addChild(mtrl);
        
        SoSphere* c = new SoSphere();
        c->radius = fPointSize;
        psep->addChild(c);
        
        ppoints = (float*)((char*)ppoints + stride);
    }

    _pFigureRoot->addChild(pparent);
    return pparent;
}

void* QtCoinViewer::_drawlinestrip(SoSeparator* pparent, const float* ppoints, int numPoints, int stride, float fwidth, const RaveVector<float>& color)
{
    if( pparent == NULL || numPoints < 2 || ppoints == NULL )
        return pparent;

    SoMaterial* mtrl = new SoMaterial;
    mtrl->diffuseColor = SbColor(color.x, color.y, color.z);
    mtrl->ambientColor = SbColor(color.x, color.y, color.z);
    mtrl->setOverride(true);
    pparent->addChild(mtrl);
    
    vector<float> mypoints((numPoints-1)*6);
    float* next;
    for(int i = 0; i < numPoints-1; ++i) {
        next = (float*)((char*)ppoints + stride);

        mypoints[6*i+0] = ppoints[0];
        mypoints[6*i+1] = ppoints[1];
        mypoints[6*i+2] = ppoints[2];
        mypoints[6*i+3] = next[0];
        mypoints[6*i+4] = next[1];
        mypoints[6*i+5] = next[2];

        ppoints = next;
    }

    SoCoordinate3* vprop = new SoCoordinate3();
    vprop->point.setValues(0,2*(numPoints-1),(float(*)[3])&mypoints[0]);
    pparent->addChild(vprop);
    
    SoDrawStyle* style = new SoDrawStyle();
    style->style = SoDrawStyle::LINES;
    style->lineWidth = fwidth;
    pparent->addChild(style);

    SoLineSet* pointset = new SoLineSet();
    vector<int> vinds(numPoints-1,2);
    pointset->numVertices.setValues(0,vinds.size(), &vinds[0]);

    pparent->addChild(pointset);
    
    _pFigureRoot->addChild(pparent);
    return pparent;
}

void* QtCoinViewer::_drawlinestrip(SoSeparator* pparent, const float* ppoints, int numPoints, int stride, float fwidth, const float* colors)
{
    if( pparent == NULL || numPoints < 2)
        return pparent;

    SoMaterial* mtrl = new SoMaterial;
    mtrl->setOverride(true);
    pparent->addChild(mtrl);

    SoMaterialBinding* pbinding = new SoMaterialBinding();
    pbinding->value = SoMaterialBinding::PER_VERTEX;
    pparent->addChild(pbinding);

    vector<float> mypoints((numPoints-1)*6), mycolors((numPoints-1)*6);
    float* next;
    for(int i = 0; i < numPoints-1; ++i) {
        next = (float*)((char*)ppoints + stride);

        mypoints[6*i+0] = ppoints[0];
        mypoints[6*i+1] = ppoints[1];
        mypoints[6*i+2] = ppoints[2];
        mypoints[6*i+3] = next[0];
        mypoints[6*i+4] = next[1];
        mypoints[6*i+5] = next[2];

        mycolors[6*i+0] = colors[3*i+0];
        mycolors[6*i+1] = colors[3*i+1];
        mycolors[6*i+2] = colors[3*i+2];
        mycolors[6*i+3] = colors[3*i+3];
        mycolors[6*i+4] = colors[3*i+4];
        mycolors[6*i+5] = colors[3*i+5];

        ppoints = next;
    }

    mtrl->diffuseColor.setValues(0, 2*(numPoints-1), (float(*)[3])&mycolors[0]);

    SoCoordinate3* vprop = new SoCoordinate3();
    vprop->point.setValues(0,2*(numPoints-1),(float(*)[3])&mypoints[0]);
    pparent->addChild(vprop);
    
    SoDrawStyle* style = new SoDrawStyle();
    style->style = SoDrawStyle::LINES;
    style->lineWidth = fwidth;
    pparent->addChild(style);

    SoLineSet* pointset = new SoLineSet();
    vector<int> vinds(numPoints-1,2);
    pointset->numVertices.setValues(0,vinds.size(), &vinds[0]);

    pparent->addChild(pointset);
    
    _pFigureRoot->addChild(pparent);
    return pparent;
}

void* QtCoinViewer::_drawlinelist(SoSeparator* pparent, const float* ppoints, int numPoints, int stride, float fwidth, const RaveVector<float>& color)
{    
    if( pparent == NULL || numPoints < 2 || ppoints == NULL )
        return pparent;

    SoMaterial* mtrl = new SoMaterial;
    mtrl->diffuseColor = SbColor(color.x, color.y, color.z);
    mtrl->ambientColor = SbColor(color.x, color.y, color.z);
    mtrl->setOverride(true);
    pparent->addChild(mtrl);
    
    vector<float> mypoints(numPoints*3);
    for(int i = 0; i < numPoints; ++i) {
        mypoints[3*i+0] = ppoints[0];
        mypoints[3*i+1] = ppoints[1];
        mypoints[3*i+2] = ppoints[2];
        ppoints = (float*)((char*)ppoints + stride);
    }

    SoCoordinate3* vprop = new SoCoordinate3();
    vprop->point.setValues(0,numPoints,(float(*)[3])&mypoints[0]);
    pparent->addChild(vprop);
    
    SoDrawStyle* style = new SoDrawStyle();
    style->style = SoDrawStyle::LINES;
    style->lineWidth = fwidth;
    pparent->addChild(style);

    SoLineSet* pointset = new SoLineSet();
    vector<int> vinds(numPoints/2,2);
    pointset->numVertices.setValues(0,vinds.size(), &vinds[0]);

    pparent->addChild(pointset);
    
    _pFigureRoot->addChild(pparent);
    return pparent;
}

void* QtCoinViewer::_drawlinelist(SoSeparator* pparent, const float* ppoints, int numPoints, int stride, float fwidth, const float* colors)
{    
    if( pparent == NULL || numPoints < 2 || ppoints == NULL )
        return pparent;

    SoMaterial* mtrl = new SoMaterial;
    mtrl->diffuseColor.setValues(0, numPoints, (float(*)[3])colors);
    mtrl->setOverride(true);
    pparent->addChild(mtrl);

    SoMaterialBinding* pbinding = new SoMaterialBinding();
    pbinding->value = SoMaterialBinding::PER_VERTEX;
    pparent->addChild(pbinding);

    vector<float> mypoints(numPoints*3);
    for(int i = 0; i < numPoints; ++i) {
        mypoints[3*i+0] = ppoints[0];
        mypoints[3*i+1] = ppoints[1];
        mypoints[3*i+2] = ppoints[2];
        ppoints = (float*)((char*)ppoints + stride);
    }

    SoCoordinate3* vprop = new SoCoordinate3();
    vprop->point.setValues(0,numPoints,(float(*)[3])&mypoints[0]);
    pparent->addChild(vprop);
    
    SoDrawStyle* style = new SoDrawStyle();
    style->style = SoDrawStyle::LINES;
    style->lineWidth = fwidth;
    pparent->addChild(style);

    SoLineSet* pointset = new SoLineSet();
    vector<int> vinds(numPoints/2,2);
    pointset->numVertices.setValues(0,vinds.size(), &vinds[0]);

    pparent->addChild(pointset);
    
    _pFigureRoot->addChild(pparent);
    return pparent;
}

void* QtCoinViewer::_drawarrow(SoSeparator* pparent, const RaveVector<float>& p1, const RaveVector<float>& p2, float fwidth, const RaveVector<float>& color)
{
    if( pparent == NULL )
        return pparent;

    SoSeparator* psep = new SoSeparator();
    SoTransform* ptrans = new SoTransform();


    SoDrawStyle* _style = new SoDrawStyle();
    _style->style = SoDrawStyle::FILLED;
    pparent->addChild(_style);  

    RaveVector<float> vrotaxis;
    RaveVector<float> direction = p2-p1;
    float fheight = sqrt(lengthsqr3(direction));

    float coneheight = fheight/10.0f;

    normalize3(direction,direction);
    //check to make sure points aren't the same
    if(sqrt(lengthsqr3(direction)) < 0.9f)
    {
        RAVEPRINT(L"QtCoinViewer::drawarrow - Error: End points are the same.\n");
        return pparent;
    }

    //rotate to face point
    cross3(vrotaxis,direction,RaveVector<float>(0,1,0));
    normalize3(vrotaxis,vrotaxis);

    float angle = -acos(dot3(direction,RaveVector<float>(0,1,0)));
    //check to make sure direction isn't pointing along y axis, if it is, don't need to rotate
    if(lengthsqr3(vrotaxis) > 0.9f)
        ptrans->rotation.setValue(SbVec3f(vrotaxis.x, vrotaxis.y, vrotaxis.z), angle); 
    
    //reusing direction vector for efficieny
    RaveVector<float> linetranslation = p1 + (fheight/2.0f-coneheight/2.0f)*direction;
    ptrans->translation.setValue(linetranslation.x, linetranslation.y, linetranslation.z);



    psep->addChild(ptrans);
    pparent->addChild(psep);

    // set a diffuse color
    SoMaterial* mtrl = new SoMaterial;
    mtrl->diffuseColor = SbColor(color.x, color.y, color.z);
    mtrl->ambientColor = SbColor(color.x, color.y, color.z);
    mtrl->setOverride(true);
    psep->addChild(mtrl);

    SoCylinder* c = new SoCylinder();
    c->radius = fwidth;
    c->height = fheight-coneheight;
    psep->addChild(c);

    //place a cone for the arrow tip

    SoCone* cn = new SoCone();
    cn->bottomRadius = fwidth;
    cn->height = coneheight;

        
    ptrans = new SoTransform();
    if(lengthsqr3(vrotaxis) > 0.9f)
        ptrans->rotation.setValue(SbVec3f(vrotaxis.x, vrotaxis.y, vrotaxis.z), angle);
    else if(fabs(fabs(angle)-M_PI) < 0.001f)
        ptrans->rotation.setValue(SbVec3f(1, 0, 0), M_PI);

    linetranslation = p2 - coneheight/2.0f*direction;
    ptrans->translation.setValue(linetranslation.x, linetranslation.y, linetranslation.z);



    psep = new SoSeparator();

    psep->addChild(mtrl);
    psep->addChild(ptrans);
    psep->addChild(cn);

    pparent->addChild(psep);

    _pFigureRoot->addChild(pparent);
    return pparent;
}

void* QtCoinViewer::_drawbox(SoSeparator* pparent, const RaveVector<float>& vpos, const RaveVector<float>& vextents)
{
    if( pparent == NULL )
        return pparent;
    RAVEPRINT(L"drawbox not implemented\n");

    _pFigureRoot->addChild(pparent);
    return pparent;
}

void* QtCoinViewer::_drawtrimesh(SoSeparator* pparent, const float* ppoints, int stride, const int* pIndices, int numTriangles, const RaveVector<float>& color)
{
    if( pparent == NULL || ppoints == NULL || numTriangles <= 0 )
        return pparent;

    SoMaterial* mtrl = new SoMaterial;
    mtrl->diffuseColor = SbColor(color.x, color.y, color.z);
    mtrl->ambientColor = SbColor(color.x, color.y, color.z);
    mtrl->transparency = 1-color.w;
    mtrl->setOverride(true);
    pparent->addChild(mtrl);

    SoMaterialBinding* pbinding = new SoMaterialBinding();
    pbinding->value = SoMaterialBinding::OVERALL;
    pparent->addChild(pbinding);

    if( color.w < 1.0f ) {
        SoTransparencyType* ptype = new SoTransparencyType();
        ptype->value = SoGLRenderAction::SORTED_OBJECT_SORTED_TRIANGLE_BLEND;
        pparent->addChild(ptype);
    }

    SoCoordinate3* vprop = new SoCoordinate3();
    
    if( pIndices != NULL ) {
        // this makes it crash!
        //vprop->point.set1Value(3*numTriangles-1,SbVec3f(0,0,0)); // resize
        for(int i = 0; i < 3*numTriangles; ++i) {
            float* p = (float*)((char*)ppoints + stride * pIndices[i]);
            vprop->point.set1Value(i, SbVec3f(p[0], p[1], p[2]));
        }
    }
    else {
        if( stride != sizeof(float)*3 ) {
            // this makes it crash!
            //vprop->point.set1Value(3*numTriangles-1,SbVec3f(0,0,0)); // resize
            for(int i = 0; i < 3*numTriangles; ++i) {
                vprop->point.set1Value(i, SbVec3f(ppoints[0], ppoints[1], ppoints[2]));
                ppoints = (float*)((char*)ppoints + stride);
            }
        }
        else {
            vprop->point.setValues(0,numTriangles*3,(float(*)[3])ppoints);
        }
    }

    pparent->addChild(vprop);

    SoFaceSet* faceset = new SoFaceSet();
    // this makes it crash!
    //faceset->numVertices.set1Value(numTriangles-1,3);
    for(int i = 0; i < numTriangles; ++i)
        faceset->numVertices.set1Value(i,3);
    //faceset->generateDefaultNormals(SoShape, SoNormalCache);

    pparent->addChild(faceset);

    _pFigureRoot->addChild(pparent);
    return pparent;
}

void QtCoinViewer::_closegraph(void* handle)
{
    if( handle != NULL ) {
        _pFigureRoot->removeChild((SoSeparator*)handle);
    }
}

#define ADD_MENU(name, checkable, shortcut, tip, fn) { \
    pact = new QAction(tr(name), this); \
    if( checkable ) pact->setCheckable(checkable); \
    if( shortcut != NULL ) pact->setShortcut(tr(shortcut)); \
    if( tip != NULL ) pact->setStatusTip(tr(tip)); \
    if( checkable )                                                   \
        connect(pact, SIGNAL(triggered(bool)), this, SLOT(fn(bool))); \
    else \
        connect(pact, SIGNAL(triggered()), this, SLOT(fn())); \
    pcurmenu->addAction(pact); \
    if( pgroup != NULL ) pgroup->addAction(pact); \
}

void QtCoinViewer::SetupMenus()
{
#if QT_VERSION >= 0x040000 // check for qt4
    QMenu* pcurmenu;
    QAction* pact;
    QActionGroup* pgroup = NULL;

    pcurmenu = menuBar()->addMenu(tr("&File"));
    ADD_MENU("Load Environment...", false, NULL, NULL, LoadEnvironment);
    ADD_MENU("Import Environment...", false, NULL, NULL, ImportEnvironment);
    ADD_MENU("Save Environment...", false, NULL, NULL, SaveEnvironment);
    pcurmenu->addSeparator();
    ADD_MENU("&Quit", false, NULL, NULL, Quit);

    pcurmenu = menuBar()->addMenu(tr("&View"));
    ADD_MENU("View Camera Params", false, NULL, NULL, ViewCameraParams);

    QMenu* psubmenu = pcurmenu->addMenu(tr("&Geometry"));
    pgroup = new QActionGroup(this);

    {
        pact = new QAction(tr("Render Only"), this);
        pact->setCheckable(true);
        pact->setChecked(_viewGeometryMode==VG_RenderOnly);
        pact->setData(VG_RenderOnly);
        psubmenu->addAction(pact);
        pgroup->addAction(pact);
    }
    {
        pact = new QAction(tr("Collision Only"), this);
        pact->setCheckable(true);
        pact->setChecked(_viewGeometryMode==VG_CollisionOnly);
        pact->setData(VG_CollisionOnly);
        psubmenu->addAction(pact);
        pgroup->addAction(pact);
    }
    {
        pact = new QAction(tr("Render/Collision"), this);
        pact->setCheckable(true);
        pact->setChecked(_viewGeometryMode==VG_RenderCollision);
        pact->setData(VG_RenderCollision);
        psubmenu->addAction(pact);
        pgroup->addAction(pact);
    }
    connect( pgroup, SIGNAL(triggered(QAction*)), this, SLOT(ViewGeometryChanged(QAction*)) );
    pgroup = NULL;

    ADD_MENU("Pubilsh Bodies Anytime", true, NULL, "Toggle publishing bodies anytime to the GUI", ViewPublishAnytime);
    pTogglePublishAnytime = pact;

    // add debug levels
    psubmenu = pcurmenu->addMenu(tr("&Debug Levels"));
    _pToggleDebug = new QActionGroup(this);

    {
        pact = new QAction(tr("Fatal"), this);
        pact->setCheckable(true);
        pact->setChecked(GetEnv()->GetDebugLevel()==Level_Fatal);
        pact->setData(Level_Fatal);
        psubmenu->addAction(pact);
        _pToggleDebug->addAction(pact);
    }
    {
        pact = new QAction(tr("Error"), this);
        pact->setCheckable(true);
        pact->setChecked(GetEnv()->GetDebugLevel()==Level_Error);
        pact->setData(Level_Error);
        psubmenu->addAction(pact);
        _pToggleDebug->addAction(pact);
    }
    {
        pact = new QAction(tr("Warn"), this);
        pact->setCheckable(true);
        pact->setChecked(GetEnv()->GetDebugLevel()==Level_Warn);
        pact->setData(Level_Warn);
        psubmenu->addAction(pact);
        _pToggleDebug->addAction(pact);
    }
    {
        pact = new QAction(tr("Info"), this);
        pact->setCheckable(true);
        pact->setChecked(GetEnv()->GetDebugLevel()==Level_Info);
        pact->setData(Level_Info);
        psubmenu->addAction(pact);
        _pToggleDebug->addAction(pact);
    }
    {
        pact = new QAction(tr("Debug"), this);
        pact->setCheckable(true);
        pact->setChecked(GetEnv()->GetDebugLevel()==Level_Debug);
        pact->setData(Level_Debug);
        psubmenu->addAction(pact);
        _pToggleDebug->addAction(pact);
    }
    {
        pact = new QAction(tr("Verbose"), this);
        pact->setCheckable(true);
        pact->setChecked(GetEnv()->GetDebugLevel()==Level_Verbose);
        pact->setData(Level_Verbose);
        psubmenu->addAction(pact);
        _pToggleDebug->addAction(pact);
    }

    connect( _pToggleDebug, SIGNAL(triggered(QAction*)), this, SLOT(ViewDebugLevelChanged(QAction*)) );

    ADD_MENU("Show Framerate", true, NULL, "Toggle showing the framerate", ViewToggleFPS);
    ADD_MENU("Show FeedBack visibility", true, NULL, "Toggle showing the axis cross", ViewToggleFeedBack);

    pcurmenu = menuBar()->addMenu(tr("&Animation"));
    ADD_MENU("Play", false, NULL, NULL, StartPlayback);
    ADD_MENU("Stop", false, NULL, NULL, StopPlayback);

    pcurmenu = menuBar()->addMenu(tr("&Options"));
    ADD_MENU("Record &Real-time Video", true, NULL, "Start recording an AVI in real clock time. Clicking this menu item again will pause the recording", RecordRealtimeVideo);
    ADD_MENU("Record &Sim-time Video", true, NULL, "Start recording an AVI in simulation time. Clicking this menu item again will pause the recording", RecordSimtimeVideo);
    
    pcurmenu = menuBar()->addMenu(tr("D&ynamics"));
    ADD_MENU("ODE Dynamic Simulation", true, NULL, NULL, DynamicSimulation);
    pToggleDynamicSimulation = pact;
    pact->setChecked(false); // physics off
    ADD_MENU("Self Collision", true, NULL, NULL, DynamicSelfCollision);
    pact->setChecked(_bSelfCollision);
    ADD_MENU("Apply Gravity", true, NULL, NULL, DynamicGravity);
    pact->setChecked(true); // gravity on

    pcurmenu = menuBar()->addMenu(tr("&Help"));
    ADD_MENU("About", false, NULL, NULL, About);

#endif
}

int QtCoinViewer::main(bool bShow)
{
    StartPlayback();
    if( bShow ) {
        _pviewer->show();
        SoQt::show(this);
    }
    SoQt::mainLoop();
    return 0;
}

void QtCoinViewer::quitmainloop()
{
    StopPlaybackTimer();
    SoQt::exitMainLoop();
}

void QtCoinViewer::actionTriggered(QAction *action)
{
    //qDebug("action '%s' triggered", action->text().toLocal8Bit().data());
}

void QtCoinViewer::InitOffscreenRenderer()
{
    // off screen target
    _ivOffscreen.setComponents(SoOffscreenRenderer::RGB);

    _pOffscreenVideo = new SoSeparator();
    _pOffscreenVideo->ref();

    // lighting model
    _pOffscreenVideo->addChild(_pviewer->getHeadlight());
    _pOffscreenVideo->addChild(_ivRoot);
    _ivRoot->ref();

    _bCanRenderOffscreen = true;
}

void QtCoinViewer::DumpIvRoot(const char *filename, bool bBinaryFile )
{
    SoOutput outfile;

    if (!outfile.openFile(filename)) {
        std::cerr << "could not open the file: " << filename << endl;
        return;
    }

    if (bBinaryFile)
        outfile.setBinary(true);

    // useful for debugging hierarchy
    SoWriteAction writeAction(&outfile);
    writeAction.apply(_ivRoot);
    outfile.closeFile();
}

void QtCoinViewer::_SelectHandler(void * userData, class SoPath * path)
{
    ((QtCoinViewer*)userData)->_HandleSelection(path);
}

void QtCoinViewer::_DeselectHandler(void * userData, class SoPath * path)
{
    ((QtCoinViewer*)userData)->_HandleDeselection(path->getTail());
}

bool QtCoinViewer::_HandleSelection(SoPath *path)
{
    Item *pItem = NULL;
    float scale = 1.0;
    bool bAllowRotation = true;

    // search the robots
    KinBody::Joint* pjoint = NULL;
    bool bIK = false;

    // for loop necessary for 3D models that include files
    SoNode* node = NULL;
    for(int i = path->getLength()-1; i >= 0; --i) {
        node = path->getNode(i);

        // search the environment
        FOREACH(it, _mapbodies) {
            assert( it->second != NULL );
            if (it->second->ContainsIvNode(node)) {
                pItem = it->second;
                break;
            }
        }

        if( pItem != NULL )
            break;
    }
        
    if (!pItem) {
        // the object cannot be selected
        return false;
    }
        
    KinBodyItem* pKinBody = dynamic_cast<KinBodyItem*>(pItem);
    KinBody::Link* pSelectedLink = pKinBody != NULL ? pKinBody->GetLinkFromIv(node) : NULL;
    int jindex=-1;
    
    if( ControlDown() ) {
        
        if( pSelectedLink != NULL ) {
            // search the joint nodes
            vector<KinBody::Joint*>::const_iterator itjoint;
            jindex = 0;
            FORIT(itjoint, pKinBody->GetBody()->GetJoints()) {
                if( pKinBody->GetBody()->DoesAffect(jindex, pSelectedLink->GetIndex()) && 
                    ((*itjoint)->GetFirstAttached()==pSelectedLink || (*itjoint)->GetSecondAttached()==pSelectedLink) ) {
                    pjoint = *itjoint;
                    break;
                }
                ++jindex;
            }
        }
    }
    
    if( pKinBody != NULL ) {
        
    }

    // construct an appropriate _pdragger
    if (pjoint != NULL) {
        _pdragger = new IvJointDragger(this, pItem, pSelectedLink->GetIndex(), scale, jindex, _bJointHilit);
    }
    else if (!bIK) {
        _pdragger = new IvObjectDragger(this, pItem, scale, bAllowRotation);
    }
    else {
        //_pdragger = new IvIKDragger(this, pItem, scale, bAxis);
    }

    _pdragger->CheckCollision(true);
    pItem->SetGrab(true);

    assert(_pSelectedItem == NULL);
    _pSelectedItem = pItem;

    // record the initially selected transform
    _initSelectionTrans = GetRaveTransform(pItem->GetIvTransform());

    return true;
}

void QtCoinViewer::_deselect()
{
    delete _pdragger; _pdragger = NULL;
    if( _pSelectedItem != NULL ) {
        _pSelectedItem->SetGrab(false);
        _pSelectedItem = NULL;
        _ivRoot->deselectAll();
    }
}

bool QtCoinViewer::_HandleDeselection(SoNode *node)
{
    delete _pdragger; _pdragger = NULL;
    
    if( _pSelectedItem != NULL ) {
        _pSelectedItem->SetGrab(false);
        _pSelectedItem = NULL;
    }
    return true;
}

// Keyboard callback handler
void QtCoinViewer::_KeyHandler(void * userData, class SoEventCallback * eventCB)
{
    QtCoinViewer* viewer = (QtCoinViewer*) userData;
    const SoEvent *event = eventCB->getEvent();

    if (SO_KEY_PRESS_EVENT(event, LEFT_ALT) )
        viewer->_altDown[0] = true;
    else if (SO_KEY_RELEASE_EVENT(event, LEFT_ALT) )
        viewer->_altDown[0] = false;
    if(SO_KEY_PRESS_EVENT(event, RIGHT_ALT))
        viewer->_altDown[1] = true;
    else if(SO_KEY_RELEASE_EVENT(event, RIGHT_ALT))
        viewer->_altDown[1] = false;

    if (SO_KEY_PRESS_EVENT(event, LEFT_CONTROL) ) {
        viewer->_ctrlDown[0] = true;
    }
    else if (SO_KEY_RELEASE_EVENT(event, LEFT_CONTROL) ) {
        viewer->_ctrlDown[0] = false;
    }

    if( SO_KEY_PRESS_EVENT(event, RIGHT_CONTROL)) {
        viewer->_ctrlDown[1] = true;
    }
    else if( SO_KEY_RELEASE_EVENT(event, RIGHT_CONTROL)) {
        viewer->_ctrlDown[1] = false;
    }
}

void QtCoinViewer::GlobAdvanceFrame(void* p, SoSensor*)
{
    assert( p != NULL );
    ((QtCoinViewer*)p)->AdvanceFrame(true);
}

//! increment the frame
void QtCoinViewer::AdvanceFrame(bool bForward)
{
    // frame counting
    static int nToNextFPSUpdate = 1;
    static int UPDATE_FRAMES = 16;
    static uint32_t basetime = timeGetTime();
    static uint32_t nFrame = 0;
    static float fFPS = 0;
    
    if( --nToNextFPSUpdate <= 0 ) {
        uint32_t newtime = timeGetTime();
        fFPS = UPDATE_FRAMES * 1000.0f / (float)max((int)(newtime-basetime),1);
        basetime = newtime;

        if( fFPS < 16 ) UPDATE_FRAMES = 4;
		else if( fFPS < 32 ) UPDATE_FRAMES = 8;
		else UPDATE_FRAMES = 16;

        nToNextFPSUpdate = UPDATE_FRAMES;
    }
    
    if( (nFrame++%16) == 0 ) {
        stringstream ss;

        if( _bDisplayFPS )
            ss << "fps: " << fixed << setprecision(2) << fFPS << endl;

        if( !_pviewer->isViewing() ) {
            MutexLock m(&_mutexMessages);
            ss << _strMouseMove;
        }

        if( _pdragger != NULL )
            _pdragger->GetMessage(ss);

        // search for all new lines
        string msg = ss.str();
        _messageNode->string.setValue("");
        int index = 0;
        std::string::size_type pos = 0, newpos=0;
        while( pos < msg.size() ) {
            newpos = msg.find('\n', pos);
            
            std::string::size_type n = newpos == std::string::npos ? msg.size()-pos : (newpos-pos);

            _messageNode->string.set1Value(index++, msg.substr(pos, n).c_str());

            if( newpos == std::string::npos )
                break;
            
            pos = newpos+1;
        }
    }

    if( _pToggleDebug != NULL )
        _pToggleDebug->actions().at(GetEnv()->GetDebugLevel())->setChecked(true);
    if( pTogglePublishAnytime != NULL )
        pTogglePublishAnytime->setChecked(GetEnv()->GetPublishBodiesAnytime());
    if( pToggleDynamicSimulation != NULL )
        pToggleDynamicSimulation->setChecked(stricmp(GetEnv()->GetPhysicsEngine()->GetXMLId(), "ODE")==0);

    MutexLock m(&_mutexUpdating);

    if( _bUpdateEnvironment ) {
        
        // process all messages
        list<EnvMessage*> listmessages;
        {
            MutexLock m(&_mutexMessages);
            listmessages.swap(_listMessages);
            assert( _listMessages.size() == 0 );
        }
        
        FOREACH(itmsg, listmessages)
            (*itmsg)->viewerexecute();
        
        // have to update model after messages since it can call LockPhysics
        UpdateFromModel();
        UpdateCameraTransform();
    }
}

void QtCoinViewer::GlobVideoFrame(void* p, SoSensor*)
{
    assert( p != NULL );
    ((QtCoinViewer*)p)->VideoFrame();
}

void QtCoinViewer::VideoFrame()
{
    if( _bSaveVideo )
        _RecordVideo();
}

void QtCoinViewer::UpdateFromModel()
{
    vector<EnvironmentBase::BODYSTATE> vecbodies;

    GetEnv()->GetPublishedBodies(vecbodies);

    FOREACH(it, _mapbodies)
        it->second->SetUserData(0);

    FOREACH(itbody, vecbodies) {
        assert( itbody->pbody != NULL );
        KinBody* pbody = itbody->pbody; // try to use only as an id, don't call any methods!

        KinBodyItem* pitem = static_cast<KinBodyItem*>(itbody->pguidata);

        if( pitem == NULL ) {
            // create a new body
            GetEnv()->LockPhysics(true);

            // make sure pbody is actually present
            if( GetEnv()->GetBodyFromNetworkId(itbody->networkid) == pbody ) {
            
                // check to make sure the real GUI data is also NULL
                if( pbody->GetGuiData() == NULL ) {
                    if( _mapbodies.find(pbody) != _mapbodies.end() ) {
                        RAVELOG(L"body %S already registered!\n", pbody->GetName());
                        GetEnv()->LockPhysics(false);
                        continue;
                    }

                    if( pbody->IsRobot() )
                        pitem = new RobotItem(this, (RobotBase*)pbody, _viewGeometryMode);
                    else
                        pitem = new KinBodyItem(this, pbody, _viewGeometryMode);
                        
                    pbody->SetGuiData(pitem);
                    GetEnv()->LockPhysics(false);
                    
                    _mapbodies[pbody] = pitem;
                }
                else {
                    pitem = static_cast<KinBodyItem*>(pbody->GetGuiData());
                    assert( _mapbodies.find(pbody) != _mapbodies.end() && _mapbodies[pbody] == pitem );
                    GetEnv()->LockPhysics(false);
                }
            }
            else {
                // body is gone
                GetEnv()->LockPhysics(false);
                continue;
            }
        }
        
        map<KinBody*, KinBodyItem*>::iterator itmap = _mapbodies.find(pbody);

        if( itmap == _mapbodies.end() ) {
            RAVELOG(L"body %S doesn't have a map associated with it!\n", itbody->strname.c_str());
            continue;
        }

        assert( pitem->GetBody() == pbody);
        assert( itmap->second == pitem );

        pitem->SetUserData(1);
        pitem->UpdateFromModel(itbody->vectrans);
    }

    FOREACH_NOINC(it, _mapbodies) {
        if( it->second->GetUserData() == 0 ) {
            // item doesn't exist anymore, remove it
            GetEnv()->LockPhysics(true);
            if( GetEnv()->GetBodyFromNetworkId(it->second->GetNetworkId()) == it->first ) {
                RAVEPRINT(L"not possible!\n");
                it->first->SetGuiData(NULL);
            }
            GetEnv()->LockPhysics(false);

            if( _pSelectedItem == it->second ) {
                delete _pdragger; _pdragger = NULL;
                _pSelectedItem = NULL;
                _ivRoot->deselectAll();
            }

            delete it->second;

            _mapbodies.erase(it++);
        }
        else
            ++it;
    }
}

void QtCoinViewer::_Reset()
{
    _deselect();

    UpdateFromModel();
    FOREACH(itbody, _mapbodies) {
        assert( itbody->first->GetGuiData() == itbody->second );
        //RAVEPRINT(L"reset %x\n", itbody->first);
        itbody->first->SetGuiData(NULL);
        delete itbody->second;
    }
    _mapbodies.clear();
}

void QtCoinViewer::UpdateCameraTransform()
{
    // set the camera depending on its mode

    // get the camera
    MutexLock m(&_mutexMessages);
    SbVec3f pos = GetCamera()->position.getValue();

    Tcam.trans = RaveVector<float>(pos[0], pos[1], pos[2]);

    SbVec3f axis;
    float fangle;
    GetCamera()->orientation.getValue(axis, fangle);
    Tcam.rotfromaxisangle(RaveVector<float>(axis[0],axis[1],axis[2]),fangle);
}

// menu items
void QtCoinViewer::LoadEnvironment()
{
#if QT_VERSION >= 0x040000 // check for qt4
    QString s = QFileDialog::getOpenFileName( this, "Load Environment", NULL, "Env Files (*.xml)");
    if( s.length() == 0 )
        return;

    bool bReschedule = false;
    if (_timerSensor->isScheduled()) {
        _timerSensor->unschedule(); 
        bReschedule = true;
    }

    _Reset();
    GetEnv()->Reset();

    GetEnv()->LockPhysics(true);
    GetEnv()->Load(s.toAscii().data());
    GetEnv()->LockPhysics(false);

    if( bReschedule ) {
        _timerSensor->schedule();
    }
#endif
}

void QtCoinViewer::ImportEnvironment()
{
#if QT_VERSION >= 0x040000 // check for qt4
    QString s = QFileDialog::getOpenFileName( this, "Load Environment", NULL, "Env Files (*.xml)");
    if( s.length() == 0 )
        return;

    GetEnv()->LockPhysics(true);
    GetEnv()->Load(s.toAscii().data());
    GetEnv()->LockPhysics(false);
#endif
}

void QtCoinViewer::SaveEnvironment()
{
#if QT_VERSION >= 0x040000 // check for qt4
    QString s = QFileDialog::getSaveFileName( this, "Save Environment", NULL, "COLLADA Files (*.dae)");
    if( s.length() == 0 )
        return;

    GetEnv()->LockPhysics(true);
    GetEnv()->Save(s.toAscii().data());
    GetEnv()->LockPhysics(false);
#endif
}

void QtCoinViewer::Quit()
{
    close();
}

void QtCoinViewer::ViewCameraParams()
{
    PrintCamera();
}

void QtCoinViewer::ViewGeometryChanged(QAction* pact)
{
    _viewGeometryMode = (ViewGeometry)pact->data().toInt();
    
    // destroy all bodies
    _deselect();

    UpdateFromModel();
    FOREACH(itbody, _mapbodies) {
        assert( itbody->first->GetGuiData() == itbody->second );
        itbody->first->SetGuiData(NULL);
        delete itbody->second;
    }
    _mapbodies.clear();
}

void QtCoinViewer::ViewPublishAnytime(bool on)
{
    GetEnv()->SetPublishBodiesAnytime(on);
}

void QtCoinViewer::ViewDebugLevelChanged(QAction* pact)
{
    GetEnv()->SetDebugLevel((DebugLevel)pact->data().toInt());
}

void QtCoinViewer::ViewToggleFPS(bool on)
{
    _bDisplayFPS = on;
    if( !_bDisplayFPS )
        _messageNode->string.setValue("");
}

void QtCoinViewer::ViewToggleFeedBack(bool on)
{
    _bDisplayFeedBack = on;
    _pviewer->setFeedbackVisibility(on);
    if( !_bDisplayFeedBack )
        _messageNode->string.setValue("Feedback Visibility OFF");
}


void QtCoinViewer::StartPlayback()
{
    _StartPlaybackTimer();
    GetEnv()->LockPhysics(true);
    GetEnv()->StartSimulation(0.002f);
    GetEnv()->LockPhysics(false);
}

void QtCoinViewer::StopPlayback()
{
    _StopPlaybackTimer();
    GetEnv()->LockPhysics(true);
    GetEnv()->StopSimulation();
    GetEnv()->LockPhysics(false);
}

void QtCoinViewer::RecordSimtimeVideo(bool on)
{
    _bRealtimeVideo = false;
    RecordSetup();
}

void QtCoinViewer::RecordRealtimeVideo(bool on)
{
    _bRealtimeVideo = true;
    RecordSetup();
}

void QtCoinViewer::RecordSetup()
{
#if QT_VERSION >= 0x040000 // check for qt4
    if( !_bSaveVideo ) {
        // start
        if( !_bAVIInit ) {
            QString s = QFileDialog::getSaveFileName( this, "Choose video filename", NULL, "AVI Files (*.avi)");
            if( s.length() == 0 )
                return;
            if( !s.endsWith(".avi", Qt::CaseInsensitive) ) s += ".avi";
            
		    if( !START_AVI((char*)s.toAscii().data(), VIDEO_FRAMERATE, VIDEO_WIDTH, VIDEO_HEIGHT, 24) ) {
                RAVELOG_ERRORA("Failed to capture %s\n", s.toAscii().data());
                return;
		    }
		    RAVELOG_DEBUGA("Starting to capture %s\n", s.toAscii().data());
		    _bAVIInit = true;
	    }
        else {
            RAVELOG_DEBUGA("Resuming previous video file\n");
        }
    }

    _bSaveVideo = !_bSaveVideo;
    SoDB::enableRealTimeSensor(!_bSaveVideo);
    SoSceneManager::enableRealTimeUpdate(!_bSaveVideo);

    _nVideoTimeOffset = 0;
    if( _bRealtimeVideo )
        _nLastVideoFrame = GetMicroTime();
    else
        _nLastVideoFrame = GetEnv()->GetSimulationTime();
#endif
}

#ifdef _DEBUG
#define GL_REPORT_ERRORD() { GLenum err = glGetError(); if( err != GL_NO_ERROR ) { RAVEPRINT(L"%s:%d: gl error 0x%x\n", __FILE__, (int)__LINE__, err); } }
#else
#define GL_REPORT_ERRORD()
#endif

bool QtCoinViewer::InitGL(int width, int height)
{
  // Get OpenGL context, set it
  QGLWidget * glwidget = (QGLWidget *)(_pviewer->getNormalWidget());
  glwidget->makeCurrent();

  // Set up GLEW, obtain function pointers
  int err = glewInit();
  if (GLEW_OK != err) {
    RAVEPRINT(L"Error initializing OpenGL extensions!\n");
    return false;
  }

  // Setup the FBO and bind it for use (needs to be reset to the default buffer (0) later)
  glGenFramebuffersEXT(1, (GLuint*)&_fb);
  glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, _fb);
  
  // Setup the output texture and bind it to the framebuffer
  glGenTextures(1, (GLuint*)&_outTex);
  glBindTexture(GL_TEXTURE_RECTANGLE_ARB, _outTex);
  glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, 4, width, height, 0, GL_RGBA, GL_FLOAT, 0); // Define the texture format
  glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST); // Turn off texture filtering
  glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST); // Turn off texture filtering
  glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_S, GL_CLAMP); // Turn off texture wrapping
  glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_T, GL_CLAMP); // Turn off texture wrapping
  glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_RECTANGLE_ARB, _outTex, 0); // Bind it to the framebuffer
  
  // Set up the renderbuffer for depth/stencil
  glGenRenderbuffersEXT(1, (GLuint*)&_rb);
  glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, _rb);
  
  // TODO: Should eventually be GL_DEPTH_STENCIL_EXT. If we don't need stencil, use GL_DEPTH_COMPONENT24
  glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_STENCIL_NV, width, height); 
  if( glGetError() != GL_NO_ERROR ) {
      // can't support stencil buffer, so try just depth
      glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT, width, height);
      if( glGetError() != GL_NO_ERROR ) {
          RAVEPRINT(L"Failed to create renderbuffer\n");
          return false;
      }
  }
  else
      glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, _rb);

  glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, _rb);

  GLenum status;
  status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
  if(status != GL_FRAMEBUFFER_COMPLETE_EXT) {
    RAVEPRINT(L"Error setting up output framebuffer\n");
    return false;
  }
  
  // Unbind texture
  glBindTexture(GL_TEXTURE_RECTANGLE_ARB, 0);

  // Restore original buffer (so OpenGL code in calling app won't draw to our framebuffer by default)
  glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);

  GLint bitsSupported;
  // check to make sure occlusion query functionality is supported
  glGetQueryiv(GL_SAMPLES_PASSED, GL_QUERY_COUNTER_BITS_ARB, &bitsSupported);
  if (bitsSupported == 0) {
    RAVEPRINT(L"Occlusion query not supported\n");
    return false;
  }

  glGenQueriesARB(1, (GLuint*)&_queryBodyAlone);
  glGenQueriesARB(1, (GLuint*)&_queryWithEnv);

  _renderAction = new SoGLRenderAction(SbViewportRegion(width, height));
  _renderAction->setTransparencyType(SoGLRenderAction::SORTED_OBJECT_BLEND);
  _renderAction->setCacheContext(SoGLCacheContextElement::getUniqueCacheContext());

  _pmycam = new SoPerspectiveCamera();
  _pmycam->ref();

  return true;
}

void QtCoinViewer::FinishGL() {
  glDeleteFramebuffersEXT(1, (GLuint*)&_fb);
  glDeleteRenderbuffersEXT(1, (GLuint*)&_rb);
  glDeleteTextures(1, (GLuint*)&_outTex);

  glDeleteQueriesARB(1, (GLuint*)&_queryBodyAlone);
  glDeleteQueriesARB(1, (GLuint*)&_queryWithEnv);

  delete _renderAction;
  _pmycam->unref();
}

bool QtCoinViewer::_GetFractionOccluded(KinBody* pbody, int width, int height, float nearPlane, float farPlane, const RaveTransform<float>& extrinsic, const float* pKK, double& fracOccluded)
{
     // Create Scenegraph of just this KinBody
    SoSeparator* root = new SoSeparator();
    root->ref();

    KinBodyItem* pitem = static_cast<KinBodyItem*>(pbody->GetGuiData());
    pitem->UpdateFromModel();
    UpdateFromModel();

    if(!_glInit) {
        if(!InitGL(width, height)) return false;
        else _glInit = true;
    }

    // Set up the camera as required
    SbViewportRegion vpr(width, height);

    vpr.setViewport(SbVec2f(pKK[2]/(float)(width/2)-1.0f, pKK[3]/(float)(height/2)-1.0f), SbVec2f(1,1));
    _renderAction->setViewportRegion(vpr);
    _pmycam->position.setValue(extrinsic.trans.x, extrinsic.trans.y, extrinsic.trans.z);
    _pmycam->orientation.setValue(extrinsic.rot.y, extrinsic.rot.z, extrinsic.rot.w, extrinsic.rot.x);
    _pmycam->aspectRatio = (pKK[0]/(float)width) / (pKK[1]/(float)height);
    _pmycam->heightAngle = atanf(2.0f*pKK[1]/(float)height);
    _pmycam->nearDistance = nearPlane;
    _pmycam->farDistance = farPlane;

    root->addChild(_pviewer->getHeadlight()); 
    root->addChild(_pmycam);
    root->addChild(_ivStyle);
    root->addChild(pitem->GetIvRoot());

    // Swap in mycam to the scenegraph
    _ivRoot->replaceChild(GetCamera(), _pmycam);
    
    // Bind to framebuffer
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, _fb);

    // Set the framebuffer as the draw target target
    glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);

    // Save OpenGL attributes
    glPushAttrib(GL_ALL_ATTRIB_BITS);

    // Clear stuff
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glClearColor(0.0, 0.0, 0.0, 0.0);
    glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

    // Render the body once
    _renderAction->apply(root);

    // Render the body we're interested in with occlusion querying.
    glDepthFunc(GL_EQUAL);
    glBeginQueryARB(GL_SAMPLES_PASSED_ARB, _queryBodyAlone);
    _renderAction->apply(root);
    glEndQueryARB(GL_SAMPLES_PASSED_ARB);

    glFlush();

    // Clear stuff
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glClearColor(0.0, 0.0, 0.0, 0.0);
    glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

    // Render whole scenegraph once
    _renderAction->apply(_pOffscreenVideo);

    // Render the body we're interested in with occlusion querying against the scenegraph we just rendered
    glDepthFunc(GL_EQUAL);
    glBeginQueryARB(GL_SAMPLES_PASSED_ARB, _queryWithEnv);
    _renderAction->apply(root);
    glEndQueryARB(GL_SAMPLES_PASSED_ARB);

    glFlush();

    // Wait until the last query (scenegraph query) we issued is done. This implies the previous one (one-body query) is also done
    do {
        glGetQueryObjectivARB(_queryWithEnv, GL_QUERY_RESULT_AVAILABLE_ARB, (GLint*)&_available);
    } while (!_available);

    // Get body query results
    glGetQueryObjectuivARB(_queryBodyAlone, GL_QUERY_RESULT_ARB, (GLuint*)&_sampleCountAlone);

    // Get body vs. scenegraph results
    glGetQueryObjectuivARB(_queryWithEnv, GL_QUERY_RESULT_ARB, (GLuint*)&_sampleCountWithEnv);

    if(_sampleCountAlone == 0)
        fracOccluded = 1;
    else
        fracOccluded = 1.0 - (double)_sampleCountWithEnv/(double)_sampleCountAlone;

    //RAVEPRINT(L"frac occluded : %.2f\n", fracOccluded*100);

    // Restore OpenGL attribute state
    glPopAttrib();

    // Restore the original draw buffer
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);

    // Delete our 'one-body' scenegraph
    root->unref();

    // Restore camera
    _ivRoot->replaceChild(_pmycam, GetCamera());

    return true;
}

bool QtCoinViewer::_GetCameraImage(void* pMemory, int width, int height, const RaveTransform<float>& _t, const float* pKK)
{
    if( pKK == NULL || pMemory == NULL || !_bCanRenderOffscreen )
        return false;

    // have to flip Z axis
    RaveTransform<float> trot; trot.rotfromaxisangle(Vector(1,0,0),PI);
    RaveTransform<float> t = _t * trot;

    SoSFVec3f position = GetCamera()->position;
    SoSFRotation orientation = GetCamera()->orientation;
    SoSFFloat aspectRatio = GetCamera()->aspectRatio;
    SoSFFloat heightAngle = GetCamera()->heightAngle;
    SoSFFloat nearDistance = GetCamera()->nearDistance;
    SoSFFloat farDistance = GetCamera()->farDistance;

    SbViewportRegion vpr(width, height);
    vpr.setViewport(SbVec2f(pKK[2]/(float)(width)-0.5f, 0.5f-pKK[3]/(float)(height)), SbVec2f(1,1));
    _ivOffscreen.setViewportRegion(vpr);

    GetCamera()->position.setValue(t.trans.x, t.trans.y, t.trans.z);
    GetCamera()->orientation.setValue(t.rot.y, t.rot.z, t.rot.w, t.rot.x);
    GetCamera()->aspectRatio = (pKK[1]/(float)height) / (pKK[0]/(float)width);
    GetCamera()->heightAngle = 2.0f*atanf(0.5f*height/pKK[1]);
    GetCamera()->nearDistance = 0.01f;
    GetCamera()->farDistance = 100.0f;
    GetCamera()->viewportMapping = SoCamera::LEAVE_ALONE;
    
    _pFigureRoot->ref();
    _ivRoot->removeChild(_pFigureRoot);
    bool bSuccess = _ivOffscreen.render(_pOffscreenVideo);
    _ivRoot->addChild(_pFigureRoot);
    _pFigureRoot->unref();

    if( bSuccess ) {
        // vertically flip since we want upper left corner to correspond to (0,0)
        for(int i = 0; i < height; ++i)
            memcpy((char*)pMemory+i*width*3, _ivOffscreen.getBuffer()+(height-i-1)*width*3, width*3);
    }
    else {
        RAVEPRINT(L"offscreen renderer failed (check video driver), disabling\n");
        _bCanRenderOffscreen = false; // need this or _ivOffscreen.render will freeze next time
    }

    GetCamera()->position = position;
    GetCamera()->orientation = orientation;
    GetCamera()->aspectRatio = aspectRatio;
    GetCamera()->heightAngle = heightAngle;
    GetCamera()->nearDistance = nearDistance;
    GetCamera()->farDistance = farDistance;
    GetCamera()->viewportMapping = SoCamera::ADJUST_CAMERA;
    return bSuccess;
}

bool QtCoinViewer::_WriteCameraImage(int width, int height, const RaveTransform<float>& _t, const float* pKK, const char* fileName, const char* extension)
{
    if( pKK == NULL || !_bCanRenderOffscreen )
        return false;

    // have to flip Z axis
    RaveTransform<float> trot; trot.rotfromaxisangle(Vector(1,0,0),PI);
    RaveTransform<float> t = _t * trot;

    SoSFVec3f position = GetCamera()->position;
    SoSFRotation orientation = GetCamera()->orientation;
    SoSFFloat aspectRatio = GetCamera()->aspectRatio;
    SoSFFloat heightAngle = GetCamera()->heightAngle;
    SoSFFloat nearDistance = GetCamera()->nearDistance;
    SoSFFloat farDistance = GetCamera()->farDistance;
    
    SbViewportRegion vpr(width, height);
    vpr.setViewport(SbVec2f(pKK[2]/(float)(width)-0.5f, 0.5f-pKK[3]/(float)(height)), SbVec2f(1,1));
    _ivOffscreen.setViewportRegion(vpr);

    GetCamera()->position.setValue(t.trans.x, t.trans.y, t.trans.z);
    GetCamera()->orientation.setValue(t.rot.y, t.rot.z, t.rot.w, t.rot.x);
    GetCamera()->aspectRatio = (pKK[1]/(float)height) / (pKK[0]/(float)width);
    GetCamera()->heightAngle = 2.0f*atanf(0.5f*height/pKK[1]);
    GetCamera()->nearDistance = 0.01f;
    GetCamera()->farDistance = 100.0f;
    GetCamera()->viewportMapping = SoCamera::LEAVE_ALONE;
    
    _pFigureRoot->ref();
    _ivRoot->removeChild(_pFigureRoot);

    bool bSuccess = true;
    if( !_ivOffscreen.render(_pOffscreenVideo) ) {
        RAVEPRINT(L"offscreen renderer failed (check video driver), disabling\n");
        _bCanRenderOffscreen = false;
        bSuccess = false;
    }
    else {
        if( !_ivOffscreen.isWriteSupported(extension) ) {
            RAVEPRINT(L"file type %s not supported, supported filetypes are\n", extension);
            wstringstream ss;
            
            for(int i = 0; i < _ivOffscreen.getNumWriteFiletypes(); ++i) {
                SbPList extlist;
                SbString fullname, description;
                _ivOffscreen.getWriteFiletypeInfo(i, extlist, fullname, description);
                ss << fullname.getString() << ": " << description.getString() << " (extensions: ";
                for (int j=0; j < extlist.getLength(); j++) {
                    ss << (const char*) extlist[j] << ", ";
                }
                ss << ")" << endl;
            }
            
            RAVEPRINT(ss.str().c_str());
            bSuccess = false;
        }
        else
            bSuccess = _ivOffscreen.writeToFile(SbString(fileName), SbString(extension));
    }

    _ivRoot->addChild(_pFigureRoot);
    _pFigureRoot->unref();

    GetCamera()->position = position;
    GetCamera()->orientation = orientation;
    GetCamera()->aspectRatio = aspectRatio;
    GetCamera()->heightAngle = heightAngle;
    GetCamera()->nearDistance = nearDistance;
    GetCamera()->farDistance = farDistance;
    GetCamera()->viewportMapping = SoCamera::ADJUST_CAMERA;
    return bSuccess;
}

bool QtCoinViewer::_RecordVideo()
{
    if( !_bAVIInit || !_bCanRenderOffscreen )
        return false;

    _ivOffscreen.setViewportRegion(SbViewportRegion(VIDEO_WIDTH, VIDEO_HEIGHT));
    _ivOffscreen.render(_pOffscreenVideo);
    
    if( _ivOffscreen.getBuffer() == NULL ) {
        RAVEPRINT(L"offset buffer null, disabling\n");
        _bCanRenderOffscreen = false;
        return false;
    }

    // flip R and B
    for(int i = 0; i < VIDEO_HEIGHT; ++i) {
        for(int j = 0; j < VIDEO_WIDTH; ++j) {
            unsigned char* ptr = _ivOffscreen.getBuffer() + 3 * (i * VIDEO_WIDTH + j);
            swap(ptr[0], ptr[2]);
        }
    }
    
    uint64_t curtime = _bRealtimeVideo ? GetMicroTime() : GetEnv()->GetSimulationTime();
    _nVideoTimeOffset += (curtime-_nLastVideoFrame);
    
    while(_nVideoTimeOffset >= (1000000/VIDEO_FRAMERATE) ) {
        if( !ADD_FRAME_FROM_DIB_TO_AVI(_ivOffscreen.getBuffer()) ) {
            RAVEPRINT(L"Failed adding frames, stopping avi recording\n");
            _bSaveVideo = false;
            SoDB::enableRealTimeSensor(!_bSaveVideo);
            SoSceneManager::enableRealTimeUpdate(!_bSaveVideo);
            return true;
        }
        
        _nVideoTimeOffset -= (1000000/VIDEO_FRAMERATE);
    }
    
    _nLastVideoFrame = curtime;

    //stuff to record camera position
        //    std::vector<RobotBase*> robots = GetEnv()->GetRobots();

    //if(0&&robots.size() != 0)
//    {
//        ofstream outfile("positions.txt",ios::app);
//        RobotBase* robot = robots[0];
//        if( robot != NULL ) {
//            RobotBase::Manipulator * pmanip = robot->GetActiveManipulator();
//            std::vector<dReal> vjointvals;
//            vjointvals.resize(robot->GetDOF());
//            robot->GetJointValues(&vjointvals[0]);
//            if(pmanip != NULL)
//            {
//                Transform etm = pmanip->GetEndEffectorTransform();
//                Transform rtm = robot->GetTransform();
//    #ifndef _WIN32
//                gettimeofday(&timestruct,NULL);
//                outfile << "\n" << timestruct.tv_sec << " " << timestruct.tv_usec << " " << "\t";
//    #endif
//
//                //end eff tm
//                outfile << etm.rot.x << " " << etm.rot.y << " " << etm.rot.z << " " << etm.rot.w << " ";
//                outfile << etm.trans.x << " " << etm.trans.y << " " << etm.trans.z << "\t";        
//
//                //robot tm
//                outfile << rtm.rot.x << " " << rtm.rot.y << " " << rtm.rot.z << " " << rtm.rot.w << " ";
//                outfile << rtm.trans.x << " " << rtm.trans.y << " " << rtm.trans.z << "\t";      
//
//                //joint vals
//                for(int i = 0; i < robot->GetDOF(); i++)
//                    outfile << " " << vjointvals[i];
//
//                outfile.close();
//                GetEnv()->plot3(etm.trans,1,0,0.004f, RaveVector<float>(1,0,0));
//            }
//        }
//    }

    return true;
}

void QtCoinViewer::DynamicSimulation(bool on)
{
    GetEnv()->SetPhysicsEngine(NULL);
    delete pphysics; pphysics = NULL;

    if( on ) {
        pphysics = GetEnv()->CreatePhysicsEngine("ode");
        GetEnv()->SetPhysicsEngine(pphysics);
    }
}

void QtCoinViewer::DynamicSelfCollision(bool on)
{
    _bSelfCollision = on;
    int opts = GetEnv()->GetPhysicsEngine()->GetPhysicsOptions();
    if( _bSelfCollision )
        opts |= PEO_SelfCollisions;
    else opts &= ~PEO_SelfCollisions;
    GetEnv()->GetPhysicsEngine()->SetPhysicsOptions(opts);
}

void QtCoinViewer::DynamicGravity(bool on)
{
    GetEnv()->GetPhysicsEngine()->SetGravity(Vector(0, 0, on?-9.8f:0));
}

void QtCoinViewer::About() {}

QtCoinViewer::EnvMessage::EnvMessage(QtCoinViewer* pviewer, void** ppreturn, bool bWaitForMutex)
    : _pviewer(pviewer), _ppreturn(ppreturn), _bWaitForMutex(bWaitForMutex)
{
    pmymutex = NULL;

    // get a mutex
    if( bWaitForMutex ) {
        if( pviewer->listMsgMutexes.size() > 0 ) {
            MutexLock m(&_pviewer->_mutexMessages);
            pmymutex = pviewer->listMsgMutexes.front();
            pviewer->listMsgMutexes.pop_front();
        }
        else {
            pmymutex = new pthread_mutex_t;
            pthread_mutex_init(pmymutex, NULL);
        }
        
        pthread_mutex_lock(pmymutex);
    }
}

QtCoinViewer::EnvMessage::~EnvMessage()
{
    assert(!_bWaitForMutex);
    
    if( _bWaitForMutex ) {
        pthread_mutex_unlock(pmymutex);
        _bWaitForMutex = false;
    }
    
    // push the mutex back
    if( pmymutex != NULL ) {
        MutexLock m(&_pviewer->_mutexMessages);
        _pviewer->listMsgMutexes.push_back(pmymutex);
    }
}

/// execute the command in the caller
void QtCoinViewer::EnvMessage::callerexecute()
{
    bool bWaitForMutex = _bWaitForMutex;
    
    {
        MutexLock m(&_pviewer->_mutexMessages);
        _pviewer->_listMessages.push_back(this);
    }
    
    if( bWaitForMutex ) {
        pthread_mutex_lock(pmymutex); // wait for it
        pthread_mutex_unlock(pmymutex);
        delete this; // yea hack, but works
    }
}

/// execute the command in the viewer
void QtCoinViewer::EnvMessage::viewerexecute()
{
    if( _bWaitForMutex ) {
        _bWaitForMutex = false;
        pthread_mutex_unlock(pmymutex);
    }
    else
        delete this; // yea, hack, but works
}