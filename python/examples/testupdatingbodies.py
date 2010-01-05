#!/usr/bin/env python
# Copyright (C) 2009-2010 Rosen Diankov (rosen.diankov@gmail.com)
# 
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#     http://www.apache.org/licenses/LICENSE-2.0
# 
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License. 
import time
from openravepy import *
from numpy import *

def run():
    print 'Example shows how to manually update the environment published bodies for the viewer while the environment is locked'
    env = Environment()
    env.Load('data/lab1.env.xml')
    env.SetViewer('qtcoin')
    robot = env.GetRobots()[0]    
    manipprob = env.CreateProblem('basemanipulation')
    env.LoadProblem(manipprob,robot.GetName())

    Tcamera = array(((0.84028,  -0.14715,   0.52179,0.930986),
                     (0.52639,   0.45182,  -0.72026,-1.233453),
                     (-0.12976,   0.87989,   0.45711,2.412977)))
    env.SetCamera(Tcamera)
    destarmangles =  array((-0.75,1.24,-0.064,2.33,-1.16,-1.548,1.19))
    env.GetViewer().EnvironmentSync()
    
    print 'Stopping the environment loop from updating the simulation'
    env.StopSimulation()
    print 'Locking environment and starting to plan'
    env.LockPhysics(True)
    manipprob.SendCommand('MoveManipulator armvals ' + ' '.join(str(f) for f in destarmangles))
    
    print 'Calling the simulation loop internally to python'
    while not robot.GetController().IsDone():
        env.StepSimulation(0.01)
        env.UpdatePublishedBodies() # used to publish body information while environment is locked
        time.sleep(0.1)
    env.LockPhysics(False)
    
    raw_input('press any key to exit: ')
    env.Destroy() # done with the environment

if __name__ == "__main__":
    run()