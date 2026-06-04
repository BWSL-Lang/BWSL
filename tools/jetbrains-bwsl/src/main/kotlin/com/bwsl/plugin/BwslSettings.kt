package com.bwsl.plugin

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.components.PersistentStateComponent
import com.intellij.openapi.components.Service
import com.intellij.openapi.components.State
import com.intellij.openapi.components.Storage

@State(name = "BwslSettings", storages = [Storage("bwsl.xml")])
@Service(Service.Level.APP)
class BwslSettings : PersistentStateComponent<BwslSettings.State> {

    data class State(var compilerPath: String = "")

    private var state = State()

    override fun getState(): State = state
    override fun loadState(state: State) { this.state = state }

    var compilerPath: String
        get() = state.compilerPath
        set(value) { state.compilerPath = value }

    companion object {
        fun getInstance(): BwslSettings =
            ApplicationManager.getApplication().getService(BwslSettings::class.java)
    }
}
