import { breakout } from './stores/BreakoutStore'

breakout.init()

breakout.lives = 1
breakout.ballY = 100000
breakout.syncBallLayout()
breakout.tick(200)
