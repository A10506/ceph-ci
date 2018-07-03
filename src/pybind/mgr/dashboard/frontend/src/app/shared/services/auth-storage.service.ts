import { Injectable } from '@angular/core';

import { Permissions } from '../models/permissions';
import { ServicesModule } from './services.module';

@Injectable({
  providedIn: ServicesModule
})
export class AuthStorageService {
  constructor() {}

  set(username: string, token: string, permissions: object = {}) {
    localStorage.setItem('dashboard_username', username);
    localStorage.setItem('access_token', token);
    localStorage.setItem('dashboard_permissions', JSON.stringify(new Permissions(permissions)));
  }

  remove() {
    localStorage.removeItem('access_token');
    localStorage.removeItem('dashboard_username');
  }

  getToken(): string {
    return localStorage.getItem('access_token');
  }

  isLoggedIn() {
    return localStorage.getItem('dashboard_username') !== null;
  }

  getPermissions(): Permissions {
    return JSON.parse(
      localStorage.getItem('dashboard_permissions') || JSON.stringify(new Permissions({}))
    );
  }
}
