import { Component, mount } from '@geastack/core'

class App extends Component {
  template() {
    return <canvas class="canvas-basic" width="64" height="32" />
  }
}

mount(App)
